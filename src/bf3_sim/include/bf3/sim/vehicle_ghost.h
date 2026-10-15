#pragma once
// The quantised vehicle ghost (docs/design/s2.md §7): the precision M3 would send for a driven vehicle, per
// research/topics/05-networking.md §7.2 (position 1/256 m x/z and 1/128 m y like the soldier ghost, smallest-three
// orientation 2 + 3 x 11 bits, linear velocity 1/64 m/s +-512, angular 1/256 rad/s +-16) plus the driver block of
// topics/04-vehicles.md §5.3 (per wheel suspension length and spin, engine rpm, gear, clutch, gearbox timers, the
// input-filter values). Lambdas, wheel contact points and the pitch-roll part are not sent: the client keeps its own
// (s2.md §7.3 restore modes). Gate G2 (s2.md §1) measures the correction error of exactly this codec; its widths are
// VehicleGhostQuantisation fields so the S2 sweep (bf3dump vehicle-replay --sweep) can vary them, and kDefault is
// what M3 adopts. The position sent is the centre-of-mass position (VehicleBodyState::com_position, the value Jolt
// integrates), so a decode writes it straight back without the origin round trip; a receiver that presents the
// frame origin computes it from the model's CoM (M3).
//
// The encoder clamps (every value maps to a code); the decoder refuses codes outside their bit width, a largest
// component index above 3, a gear outside the model's range and a wheel count above kMaxVehicleWheels with an error
// (hostile network input, s2.md §9). Quantisers work in double like bf3_net's Quantizer (bit_stream.h), whose
// position quantisers these equal, so M3 can send the position with the soldier ghost's code path.

#include "bf3/core/result.h"
#include "bf3/sim/vehicle_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace bf3::sim {

// A uniform quantiser: value -> integer in [0, 2^bits), clamped (NaN -> 0); decode(encode(v)) is within step / 2 of
// v in range. Same arithmetic as bf3::net::Quantizer.
struct GhostQuantizer {
	float min = 0;
	float step = 1;
	unsigned bits = 0;

	constexpr uint32_t max_code() const { return bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u); }
	constexpr uint32_t encode(float p_value) const {
		const double t = (double(p_value) - double(min)) / double(step) + 0.5;
		if (!(t > 0.0)) {
			return 0;
		}
		const double top = double(max_code());
		return t >= top ? max_code() : uint32_t(t);
	}
	constexpr float decode(uint32_t p_code) const { return min + float(p_code > max_code() ? max_code() : p_code) * step; }
};

struct VehicleGhostQuantisation {
	GhostQuantizer position_xz{ -4096.0f, 1.0f / 256.0f, 21 }; // = bf3::net::quant::kPositionXZ
	GhostQuantizer position_y{ -256.0f, 1.0f / 128.0f, 18 }; // = bf3::net::quant::kPositionY
	unsigned orientation_bits = 11; // per smallest-three component (+ 2 bits for the largest index)
	GhostQuantizer linear_velocity{ -512.0f, 1.0f / 64.0f, 16 };
	GhostQuantizer angular_velocity{ -16.0f, 1.0f / 256.0f, 13 };
	// driver block (s2.md §7.2)
	unsigned suspension_bits = 16; // over [0, the wheel's suspension max length]
	GhostQuantizer wheel_spin{ -256.0f, 1.0f / 64.0f, 15 }; // rad/s (150 km/h on r 0.46 = 91 rad/s)
	unsigned wheel_angle_bits = 8; // visual spin angle over one turn (presentation only)
	GhostQuantizer engine_rpm{ 0.0f, 0.25f, 15 }; // 0..8191.75 rpm
	unsigned gear_bits = 4; // gear + 8, -8..7
	GhostQuantizer clutch{ 0.0f, 1.0f / 255.0f, 8 };
	GhostQuantizer gearbox_timer{ 0.0f, 1.0f / 256.0f, 9 }; // 0..2 s, each of the three timers
	GhostQuantizer driver_filter{ -1.0f, 1.0f / 2047.0f, 12 }; // steer / throttle filters; brake on [0, 1] uses the upper half
	unsigned stand_still_bits = 10; // ticks, saturating
	bool send_controller_inputs = false; // the four Jolt inputs are re-derived from the command and the driver state
	// The wheel collision test reruns every step (mNumStepsBetweenCollisionTestActive / Inactive = 1,
	// VehicleConstraint.h:226-227) and rewrites each wheel's suspension length before the step reads it, so the
	// suspension codes should carry no physics state. The sweep's "--no-suspension" row (false) confirms it; if G2 is
	// unchanged M3 drops the 64 bits from the driven-vehicle row (s2.md §7.3).
	bool send_suspension = true;

	bool operator==(const VehicleGhostQuantisation &) const = default;
};
inline constexpr VehicleGhostQuantisation kDefaultVehicleGhost{};

// The codes of one ghost, unpacked (the wire packing is M3's; total_bits says what it costs).
struct VehicleGhostCodes {
	uint32_t position[3] = {};
	uint8_t orientation_largest = 0; // 0..3: which quaternion component was dropped (sign made positive)
	uint16_t orientation[3] = {};
	uint32_t linear_velocity[3] = {};
	uint32_t angular_velocity[3] = {};
	uint8_t active = 1;
	uint8_t wheel_count = 0;
	std::array<uint32_t, kMaxVehicleWheels> suspension{};
	std::array<uint32_t, kMaxVehicleWheels> wheel_spin{};
	std::array<uint32_t, kMaxVehicleWheels> wheel_angle{};
	uint32_t engine_rpm = 0;
	uint32_t gear = 0;
	uint32_t clutch = 0;
	uint32_t gearbox_timers[3] = {};
	uint32_t steer_filter = 0, throttle_filter = 0, brake_filter = 0;
	uint32_t stand_still = 0;
	uint8_t reversing = 0;
};

// Bits one ghost of p_wheels wheels costs at p_q (the Humvee at kDefault: 183 body bits + 4 x 39 wheel bits + 101
// controller / driver bits = 440 bits, 55 bytes; without the suspension codes 376 bits, 47 bytes; s2.md §7.2).
size_t vehicle_ghost_bits(const VehicleGhostQuantisation &p_q, size_t p_wheels);

// Per-wheel suspension max length, needed to scale the suspension codes (VehicleModel::wheels[i].suspension_max).
struct VehicleGhostScale {
	std::array<float, kMaxVehicleWheels> suspension_max{};
	int32_t min_gear = -8, max_gear = 7; // the model's transmission range
};

VehicleGhostCodes encode_vehicle_ghost(const VehicleState &p_state, const VehicleGhostQuantisation &p_q, const VehicleGhostScale &p_scale);
// Decodes into r_state: overwrites the body's vehicle_restore::kBody fields (com_position, rotation, velocities,
// active), the controller's rpm / gear / clutch / timers, the wheels' spin, angle and (if sent) suspension length and
// the driver state; leaves every other field of r_state (the sleep test, previous_delta_time, lambdas, contacts,
// pitch-roll, controller inputs unless sent, current_step) as it was. Callers decode into a copy of the client
// world's own state(slot), so those fields are the client's own (s2.md §7.3); body.origin is not recomputed.
core::Status decode_vehicle_ghost(const VehicleGhostCodes &p_codes, const VehicleGhostQuantisation &p_q, const VehicleGhostScale &p_scale,
		VehicleState &r_state);

// Smallest-three quaternion (x, y, z, w): drop the largest-magnitude component (made positive), code the other three
// over [-1/sqrt2, 1/sqrt2] with p_bits each. decode renormalises with a correctly rounded sqrt (no libm trig;
// deterministic under /fp:precise).
void encode_quat_smallest_three(const std::array<float, 4> &p_q, unsigned p_bits, uint8_t &r_largest, uint16_t r_codes[3]);
std::array<float, 4> decode_quat_smallest_three(uint8_t p_largest, const uint16_t p_codes[3], unsigned p_bits);

} // namespace bf3::sim
