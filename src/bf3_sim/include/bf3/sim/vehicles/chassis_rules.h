#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace bf3::sim::vehicles {

inline constexpr float kChassisGravity = 9.82f;
inline constexpr float kSteerSensitivityClamp = 0.5f;
inline constexpr float kSteerHalfTurn = 3.14159265358979323846f;
inline constexpr float kLossKmhPerStrength = 100.0f;
inline constexpr float kLossDivisor = 500.0f;
inline constexpr float kEngineBrakeScale = 0.01f;
inline constexpr float kEngineBrakeFraction = 0.03125f;
inline constexpr float kEngineBrakeFadeSpeed = 10.0f;
inline constexpr float kRollingSpinScale = 10.0f;
inline constexpr float kRollingClamp = 2.0f;
inline constexpr float kLowSpeedAutoBrakeSpeed = 1.0f;
inline constexpr float kMotionDampingRate = 10.0f;
inline constexpr float kAntiRollGain = 2.0f;
inline constexpr float kAntiRollCap = 6.0f;
inline constexpr float kAntiRollSameAxle = 0.1f;
inline constexpr float kSubstepLowSpeedKmh = 15.0f;
inline constexpr uint32_t kMaxChassisSubsteps = 3;
inline constexpr float kHalf = 0.5f;
inline constexpr float kChassisTickHz = 30.0f;
inline constexpr float kProgressiveSetting = 1.0f;
inline constexpr float kDamperClamp = 0.8f;

using CurvePoint = std::array<float, 2>;

float steer_curve_at(std::span<const CurvePoint> p_points, float p_key);

float loss_curve_at(std::span<const CurvePoint> p_points, float p_key);

uint32_t chassis_substeps(float p_speed, bool p_wheeled);

float spring_constant(float p_mass, uint32_t p_wheels, float p_stiffness, float p_length);

float damper_coefficient(float p_mass, uint32_t p_wheels, float p_damping, float p_progressive, float p_substep_dt);

float damper_clamped_coefficient(float p_coefficient, float p_spring_force, float p_extension_speed);

float steer_sensitivity(std::span<const CurvePoint> p_points, float p_speed, bool p_inverted);

float steer_angle(float p_yaw, float p_sensitivity);

float transmission_loss_resistance(
		std::span<const CurvePoint> p_points, float p_planar_speed, float p_strength, float p_mass, uint32_t p_wheels);

float engine_brake_resistance(float p_mass, float p_speed);

float rolling_resistance(float p_spin, float p_base_factor, float p_velocity_factor_kmh, float p_width,
		float p_wheel_mass, uint32_t p_wheels);

float spin_after_resistance(float p_spin, float p_resistance, float p_dt, float p_inertia);

struct MotionDampingRule {
	float pitch = 0, yaw = 0, roll = 0, linear = 0;
	std::array<float, 3> linear_modifier{ 1, 1, 1 };
};

std::array<float, 3> motion_damping_angular(const MotionDampingRule &p_rule, float p_dt);

std::array<float, 3> motion_damping_linear(const MotionDampingRule &p_rule, float p_dt);

struct WindRule {
	float coefficient_of_air_friction = 0, air_drag_area = 0, air_density = 0;
	float base_factor = 0, velocity_factor_kmh = 0, velocity_factor_min = 0, velocity_factor_max = 0;
};

float wind_drag(const WindRule &p_rule, float p_speed);

float down_force(float p_speed, float p_air_density, float p_base_factor, bool p_use_wheel_factor, float p_wheel_factor,
		uint32_t p_wheels, uint32_t p_grounded);

std::array<float, 2> anti_roll_loads(float p_load_a, float p_load_b, float p_stiffness);

}
