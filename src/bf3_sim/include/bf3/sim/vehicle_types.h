#pragma once
// Plain vehicle types shared by the vehicle world, the replay harness, the ghost codec and (M3/M4) the protocol and
// the presentation (docs/design/s2.md §6). No Jolt and no Godot here.
//
// VehicleCommand: one tick of a driver's input, quantised where it is made (like Command, command.h): the client
// predicts with exactly the numbers the server receives. VehicleState: everything the vehicle world's step reads
// from one vehicle besides the static world and the contact cache, in full precision: the Jolt body
// (Body::SaveState + MotionProperties::SaveState, Body.cpp:296, MotionProperties.cpp:63, including the sleep test
// state), the VehicleConstraint (wheels, lambdas, pitch-roll part), the WheeledVehicleController (inputs, engine,
// transmission) and our own driver state (input filter, stand-still timer). Field for field the values Jolt's own
// streams hold (PhysicsSystem::SaveBodyState, VehicleConstraint::SaveState: VehicleConstraint.cpp:646,
// WheeledVehicleController.cpp:827, VehicleEngine.cpp:112, VehicleTransmission.cpp:141), so a VehicleState restored
// through VehicleWorld::restore_vehicle_state with vehicle_restore::kAll writes back exactly the bytes Jolt saved:
// the vehicle's part of the world is then bit-identical to a Jolt restore of it, and only the contact cache (kept or
// invalidated, ContactCachePolicy) can differ (s2.md §6.4). The body position is the centre-of-mass position
// (Body::mPosition, what Jolt integrates and saves), never the frame origin: origin = com - R * CoM is not
// invertible bit-exactly in float (Body.h:273, Body.cpp:106), so the origin is a reported-only field.

#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace bf3::sim {

// Physics wheels per vehicle (wptNormal wheels; the Humvee has 4, BTR-90 / LAV-25 8). More is a model error.
inline constexpr size_t kMaxVehicleWheels = 8;
// "No body" in VehicleWheelState::contact_body (JPH::BodyID::cInvalidBodyID).
inline constexpr uint32_t kNoContactBody = 0xFFFFFFFFu;

// The byte sizes of the two Jolt streams restore_vehicle_state writes (Jolt 5.6.0, single precision; s2.md §6.4).
// The implementation checks Jolt's own SaveBodyState / VehicleConstraint::SaveState against them before patching.
// Body: active 1, position 12, rotation 16, linear 12, angular 12, force 12, torque 12, sleep spheres 3 x 16,
// sleep timer 4, allow sleeping 1 (BodyManager.cpp:831, Body.cpp:296, MotionProperties.cpp:63).
inline constexpr size_t kJoltBodyStreamBytes = 130;
// VehicleConstraint: mEnabled 1, controller 44, 68 per wheel, pitch-roll axis 12 + lambda 4 + mCurrentStep 4.
constexpr size_t jolt_vehicle_constraint_stream_bytes(size_t p_wheels) { return 1 + 44 + 68 * p_wheels + 20; }

namespace vehicle_buttons {
inline constexpr uint8_t kHandbrake = 1u << 0; // held
inline constexpr uint8_t kBoost = 1u << 1; // reserved (S5: Boost data is 1.0 on the Humvee)
inline constexpr uint8_t kHorn = 1u << 2; // reserved (cosmetic)
inline constexpr uint8_t kWireMask = 0x07u;
} // namespace vehicle_buttons

struct VehicleCommand {
	Tick tick = 0; // the server tick this command drives
	uint32_t sequence = 0; // +1 per command of one client session
	int8_t throttle = 0; // -127..127: > 0 forward, < 0 reverse (AllowNegativeThrottle); -128 never produced
	uint8_t brake = 0; // 0..255
	// -127..127: > 0 steers right in the project's axes (right = -X at yaw 0, command.h / soldier_motor.cpp), which
	// is Jolt's right (forward x up), so it reaches SetDriverInput's right input without a sign flip (s2.md §5.4)
	int8_t steer = 0;
	uint8_t buttons = 0; // vehicle_buttons::
	uint8_t flags = 0; // command_flags:: (server side only; the step never reads it)

	bool operator==(const VehicleCommand &) const = default;
};

// One of MotionProperties::mSleepTestSpheres (Sphere: Float3 centre + radius, 16 bytes in the stream).
struct VehicleSleepSphere {
	Vec3 center{};
	float radius = 0;
};

// The Jolt body (JPH::Body + MotionProperties), Frostbite axes, in PhysicsSystem::SaveBodyState's order (single
// precision: active bool 1, position 12, rotation 16, linear 12, angular 12, force 12, torque 12, sleep spheres 48,
// sleep timer 4, allow sleeping 1 = 130 bytes). rotation is a quaternion (x, y, z, w).
struct VehicleBodyState {
	Vec3 com_position{}; // Body::GetCenterOfMassPosition (= Body::mPosition)
	std::array<float, 4> rotation{ 0, 0, 0, 1 };
	Vec3 linear_velocity{};
	Vec3 angular_velocity{};
	// vehicle_restore::kBodySleep: accumulated force / torque (zero after every step, PhysicsSystem.cpp:2541) and
	// the sleep test (Body::UpdateSleepStateInternal). A BodyInterface position or velocity setter would reset the
	// sleep test; restore_vehicle_state never uses one.
	Vec3 force{};
	Vec3 torque{};
	std::array<VehicleSleepSphere, 3> sleep_test_spheres{};
	float sleep_test_timer = 0;
	uint8_t active = 1; // Body::IsActive (0 = asleep); vehicle_restore::kBody
	uint8_t allow_sleeping = 1; // MotionProperties::mAllowSleeping; vehicle_restore::kBodySleep
	uint8_t reserved[2] = {};
	Vec3 origin{}; // reported only: Body::GetPosition, the vehicle frame origin (what gate G2 measures)
};

// One VehicleConstraint wheel (Wheel / WheelWV). steer_angle is derived every step from the controller's right input
// (WheeledVehicleController::PreCollide) and is reported, not restored.
struct VehicleWheelState {
	float angular_velocity = 0; // rad/s
	float angle = 0; // rad, the visual spin angle
	float suspension_length = 0; // m, attachment to wheel centre
	uint32_t contact_body = kNoContactBody; // BodyID value
	Vec3 contact_position{};
	Vec3 contact_normal{};
	Vec3 contact_lateral{};
	// AxisConstraintPart totalLambda of the suspension, suspension-max-up, longitudinal and lateral parts
	float lambda_suspension = 0;
	float lambda_suspension_max_up = 0;
	float lambda_longitudinal = 0;
	float lambda_lateral = 0;
	float steer_angle = 0; // reported only
	uint8_t in_contact = 0; // reported only (Wheel::HasContact after the step)
	uint8_t reserved[3] = {};
};

// WheeledVehicleController + VehicleEngine + VehicleTransmission.
struct VehicleControllerState {
	float forward_input = 0;
	float right_input = 0;
	float brake_input = 0;
	float hand_brake_input = 0;
	float previous_delta_time = 0;
	float engine_rpm = 0;
	int32_t gear = 0; // -n..-1 reverse, 0 neutral, 1..n forward
	float clutch = 0; // mClutchFriction 0..1
	float gear_switch_time_left = 0;
	float clutch_release_time_left = 0;
	float gear_switch_latency_time_left = 0;
};

// Our own per-vehicle driver state (vehicle_model.h VehicleDriver; s2.md §5.4), stepped inside the tick.
struct VehicleDriverState {
	float steer_filter = 0; // -1..1: the YawInertia-filtered steering input
	float throttle_filter = 0; // -1..1 (Humvee ThrottleInertia 0: equals the input)
	float brake_filter = 0; // 0..1
	uint16_t stand_still_ticks = 0; // ticks at low speed without throttle (UseStandStillBrake)
	uint8_t reversing = 0; // the driver asked for reverse and the gearbox took it
	uint8_t reserved = 0;
};

struct VehicleState {
	Tick tick = 0; // the tick this state is the end of
	VehicleBodyState body;
	VehicleControllerState controller;
	VehicleDriverState driver;
	std::array<VehicleWheelState, kMaxVehicleWheels> wheels{};
	uint8_t wheel_count = 0;
	uint8_t constraint_enabled = 1; // Constraint::mEnabled
	uint8_t reserved[2] = {};
	Vec3 pitch_roll_axis{}; // mPitchRollRotationAxis
	float pitch_roll_lambda = 0; // mPitchRollPart totalLambda
	uint32_t current_step = 0; // VehicleConstraint::mCurrentStep

	bool operator==(const VehicleState &) const = default;
};

static_assert(std::is_trivially_copyable_v<VehicleCommand>);
static_assert(std::is_trivially_copyable_v<VehicleState>);

// FNV-1a 64 over the restorable fields' bit patterns in declaration order, the first wheel_count wheels only, never
// the padding (reserved bytes), the reported-only fields (body.origin, steer_angle, in_contact) or tick. -0.0 and
// 0.0 hash differently: the gate is bit identity (s2.md §1 G1).
uint64_t vehicle_state_hash(const VehicleState &p_state);

// The first field two states differ in, as "body.com_position.y", "wheels[2].lambda_lateral", "controller.gear", ...;
// empty when every hashed field is bit-equal (the G1 "measure why" report, s2.md §1 fail rule).
std::string vehicle_state_first_difference(const VehicleState &p_a, const VehicleState &p_b);

} // namespace bf3::sim
