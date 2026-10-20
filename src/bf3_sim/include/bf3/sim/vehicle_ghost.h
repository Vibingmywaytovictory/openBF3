#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/vehicle_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace bf3::sim {

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
	GhostQuantizer position_xz{ -4096.0f, 1.0f / 256.0f, 21 };
	GhostQuantizer position_y{ -256.0f, 1.0f / 128.0f, 18 };
	unsigned orientation_bits = 11;
	GhostQuantizer linear_velocity{ -512.0f, 1.0f / 64.0f, 16 };
	GhostQuantizer angular_velocity{ -16.0f, 1.0f / 256.0f, 13 };

	unsigned suspension_bits = 16;
	GhostQuantizer wheel_spin{ -256.0f, 1.0f / 64.0f, 15 };
	unsigned wheel_angle_bits = 8;
	GhostQuantizer engine_rpm{ 0.0f, 0.25f, 15 };
	unsigned gear_bits = 4;
	GhostQuantizer clutch{ 0.0f, 1.0f / 255.0f, 8 };
	GhostQuantizer gearbox_timer{ 0.0f, 1.0f / 256.0f, 9 };
	GhostQuantizer driver_filter{ -1.0f, 1.0f / 2047.0f, 12 };
	unsigned stand_still_bits = 10;
	bool send_controller_inputs = false;

	bool send_suspension = true;

	bool operator==(const VehicleGhostQuantisation &) const = default;
};
inline constexpr VehicleGhostQuantisation kDefaultVehicleGhost{};

struct VehicleGhostCodes {
	uint32_t position[3] = {};
	uint8_t orientation_largest = 0;
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

	uint32_t controller_inputs[4] = {};
};

size_t vehicle_ghost_bits(const VehicleGhostQuantisation &p_q, size_t p_wheels);

struct VehicleGhostScale {
	uint8_t wheel_count = 0;
	std::array<float, kMaxVehicleWheels> suspension_max{};
	int32_t min_gear = -8, max_gear = 7;
};
struct VehicleModel;
VehicleGhostScale vehicle_ghost_scale(const VehicleModel &p_model);

VehicleGhostCodes encode_vehicle_ghost(const VehicleState &p_state, const VehicleGhostQuantisation &p_q, const VehicleGhostScale &p_scale);

core::Status decode_vehicle_ghost(const VehicleGhostCodes &p_codes, const VehicleGhostQuantisation &p_q, const VehicleGhostScale &p_scale,
		VehicleState &r_state);

void encode_quat_smallest_three(const std::array<float, 4> &p_q, unsigned p_bits, uint8_t &r_largest, uint16_t r_codes[3]);
core::Result<std::array<float, 4>> decode_quat_smallest_three(uint8_t p_largest, const uint16_t p_codes[3], unsigned p_bits);

}
