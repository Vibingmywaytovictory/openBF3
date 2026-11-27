#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bf3::sim::vehicles {

inline constexpr float kDeadzoneReduction = 0.15f;
inline constexpr float kInstantInertiaStep = 2.0f;
inline constexpr float kShiftInputThreshold = 0.0625f;
inline constexpr float kStopSpeed = 1.0f;
inline constexpr float kKmhPerMs = 3.6f;
inline constexpr float kManualTapLatch = 0.75f;
inline constexpr float kRadPerSecToRpm = 9.549296f;
inline constexpr float kMinRpmStep = 1.0f;
inline constexpr float kStandStillSpeedSquared = 7.71605f;
inline constexpr float kStandStillInput = 0.25f;
inline constexpr float kSlipClamp = 200.0f;
inline constexpr float kDriveTorqueShare = 0.075f;
inline constexpr float kDriveTorqueQuarter = 0.25f;

struct InputAxisRule {
	float deadzone = 0;
	float inertia_in = 0;
	float inertia_out = 0;
	float inertia_min_ratio = 0;
};

float shape_deadzone(float p_input, float p_deadzone, bool p_signed);

float shape_inertia(float p_previous, float p_target, float p_dt, const InputAxisRule &p_axis);

float torque_curve_at(float p_rpm, std::span<const float> p_rpm_points, std::span<const float> p_torque_points);

float engine_torque(float p_rpm, std::span<const float> p_rpm_points, std::span<const float> p_torque_points,
		float p_power_multiplier, float p_throttle, float p_clutch);

struct GearboxRule {
	std::span<const float> forward_speeds;
	std::span<const float> reverse_speeds;
	int32_t forward_gears = 0;
	int32_t reverse_gears = 0;
	int32_t gearbox_type = 0;
	int32_t gearbox_mode = 0;
	float gear_change_time = 0;
	float gear_down_speed_factor = 0;
	float opposite_dir_gear_change_max_speed = 0;
	float opposite_dir_gear_change_time = 0;
	float clutch_speed_factor = 0;
	bool use_neutral_gear = false;
	bool use_classic_auto_clutch = false;
};

enum class GearPhase : uint8_t {
	Idle = 0,
	Disengage = 1,
	Wait = 2,
	Reengage = 3,
	OppositeDirection = 5,
};

enum class GearRequest : uint8_t { None = 0, Up = 1, Down = 2, OppositeDirection = 3 };

struct GearboxState {
	int32_t gear = 1;
	GearPhase phase = GearPhase::Idle;
	int8_t pending = 0;
	uint8_t reverse_direction = 0;
	uint8_t reserved = 0;
	float clutch = 0;
	float timer = 0;

	bool operator==(const GearboxState &) const = default;
};

GearRequest gearbox_request(
		const GearboxRule &p_rule, const GearboxState &p_state, float p_throttle, float p_brake, float p_speed);

bool gearbox_accept(const GearboxRule &p_rule, GearboxState &r_state, GearRequest p_request);

void gearbox_advance(const GearboxRule &p_rule, GearboxState &r_state, float p_dt);

bool gearbox_clutch_pressed(const GearboxRule &p_rule, const GearboxState &p_state);

int32_t gear_step(const GearboxRule &p_rule, int32_t p_gear, int32_t p_delta);

float wheel_rpm(std::span<const float> p_wheel_angular_velocities, float p_gear_ratio, float p_diff_gear_ratio,
		float p_transmission_efficiency);

struct EngineRule {
	float rpm_min = 0;
	float rpm_max = 0;
	float internal_acceleration = 0;
	float internal_deceleration = 0;
	int32_t gearbox_mode = 0;
};

float engine_rpm_target(const EngineRule &p_rule, bool p_in_gear, bool p_clutch_pressed, float p_clutch,
		float p_throttle, float p_wheel_rpm);

float engine_load(float p_wheel_rpm, float p_rpm, float p_forward_y, float p_forward_speed_sign);

float engine_rpm_desired(float p_target, float p_load, float p_floor);

float engine_rpm_step(const EngineRule &p_rule, float p_rpm, float p_desired, float p_floor, float p_dt);

float wheel_drive_torque(float p_engine_torque, float p_gear_ratio, float p_diff_gear_ratio,
		float p_transmission_efficiency, float p_share);

float spring_substep_factor(uint32_t p_substeps);

float spring_progressive(float p_compression_ratio, float p_start_ratio, float p_exponent, float p_setting);

float brake_scale(float p_angular_velocity, float p_factor, float p_input);

float chassis_friction(float p_horizontal_speed_squared, float p_input, float p_normal, float p_low_velocity);

struct EllipseForces {
	float longitudinal = 0;
	float lateral = 0;
};

EllipseForces friction_ellipse(float p_longitudinal, float p_lateral, float p_longitudinal_peak, float p_lateral_peak);

float clamp_slip(float p_slip);

struct BoostRule {
	float dissipation_time = 0;
	float recovery_time = 0;
};

float boost_tank_step(const BoostRule &p_rule, float p_tank, bool p_boosting, float p_dt);

}
