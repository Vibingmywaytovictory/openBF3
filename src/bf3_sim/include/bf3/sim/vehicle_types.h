#pragma once

#include "bf3/sim/sim_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace bf3::sim {

inline constexpr size_t kMaxVehicleWheels = 8;

inline constexpr uint32_t kNoContactBody = 0xFFFFFFFFu;

inline constexpr size_t kJoltBodyStreamBytes = 130;

constexpr size_t jolt_vehicle_constraint_stream_bytes(size_t p_wheels) { return 1 + 44 + 68 * p_wheels + 20; }

namespace vehicle_buttons {
inline constexpr uint8_t kHandbrake = 1u << 0;
inline constexpr uint8_t kBoost = 1u << 1;
inline constexpr uint8_t kHorn = 1u << 2;
inline constexpr uint8_t kWireMask = 0x07u;
}

struct VehicleCommand {
	Tick tick = 0;
	uint32_t sequence = 0;
	int8_t throttle = 0;
	uint8_t brake = 0;

	int8_t steer = 0;
	uint8_t buttons = 0;
	uint8_t flags = 0;

	bool operator==(const VehicleCommand &) const = default;
};

struct VehicleSleepSphere {
	Vec3 center{};
	float radius = 0;

	bool operator==(const VehicleSleepSphere &) const = default;
};

struct VehicleBodyState {
	Vec3 com_position{};
	std::array<float, 4> rotation{ 0, 0, 0, 1 };
	Vec3 linear_velocity{};
	Vec3 angular_velocity{};

	Vec3 force{};
	Vec3 torque{};
	std::array<VehicleSleepSphere, 3> sleep_test_spheres{};
	float sleep_test_timer = 0;
	uint8_t active = 1;
	uint8_t allow_sleeping = 1;
	uint8_t reserved[2] = {};
	Vec3 origin{};

	bool operator==(const VehicleBodyState &) const = default;
};

struct VehicleWheelState {
	float angular_velocity = 0;
	float angle = 0;
	float suspension_length = 0;
	uint32_t contact_body = kNoContactBody;
	Vec3 contact_position{};
	Vec3 contact_normal{};
	Vec3 contact_lateral{};

	float lambda_suspension = 0;
	float lambda_suspension_max_up = 0;
	float lambda_longitudinal = 0;
	float lambda_lateral = 0;
	float steer_angle = 0;
	uint8_t in_contact = 0;
	uint8_t reserved[3] = {};

	bool operator==(const VehicleWheelState &) const = default;
};

struct VehicleControllerState {
	float forward_input = 0;
	float right_input = 0;
	float brake_input = 0;
	float hand_brake_input = 0;
	float previous_delta_time = 0;
	float engine_rpm = 0;
	int32_t gear = 0;
	float clutch = 0;
	float gear_switch_time_left = 0;
	float clutch_release_time_left = 0;
	float gear_switch_latency_time_left = 0;

	bool operator==(const VehicleControllerState &) const = default;
};

struct VehicleDriverState {
	float steer_filter = 0;
	float throttle_filter = 0;
	float brake_filter = 0;
	uint16_t stand_still_ticks = 0;
	uint8_t reversing = 0;
	uint8_t reserved = 0;

	bool operator==(const VehicleDriverState &) const = default;
};

struct VehicleState {
	Tick tick = 0;
	VehicleBodyState body;
	VehicleControllerState controller;
	VehicleDriverState driver;
	std::array<VehicleWheelState, kMaxVehicleWheels> wheels{};
	uint8_t wheel_count = 0;
	uint8_t constraint_enabled = 1;
	uint8_t reserved[2] = {};
	Vec3 pitch_roll_axis{};
	float pitch_roll_lambda = 0;
	uint32_t current_step = 0;

	bool operator==(const VehicleState &) const = default;
};

static_assert(std::is_trivially_copyable_v<VehicleCommand>);
static_assert(std::is_trivially_copyable_v<VehicleState>);

uint64_t vehicle_state_hash(const VehicleState &p_state);

std::string vehicle_state_first_difference(const VehicleState &p_a, const VehicleState &p_b);

}
