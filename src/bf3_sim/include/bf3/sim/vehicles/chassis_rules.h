#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace bf3::sim::vehicles {

// SOURCE: notes R5-G1-2 (9.82 m/s^2 in K) and R6-G1-apply 4 (the slope term).
inline constexpr float kChassisGravity = 9.82f;
// SOURCE: notes R5-G1-3 (sensitivity clamped to 0.5; the steer value is in half-turns, times pi).
inline constexpr float kSteerSensitivityClamp = 0.5f;
inline constexpr float kSteerHalfTurn = 3.14159265358979323846f;
// SOURCE: notes R5-G1-1 3 (TransmissionLoss key shift 100 km/h per unit of strength, divisor 500).
inline constexpr float kLossKmhPerStrength = 100.0f;
inline constexpr float kLossDivisor = 500.0f;
// SOURCE: notes R5-G1-1 4 (engine brake 0.01, 0.03125, f = 11 - v up to 10 m/s).
inline constexpr float kEngineBrakeScale = 0.01f;
inline constexpr float kEngineBrakeFraction = 0.03125f;
inline constexpr float kEngineBrakeFadeSpeed = 10.0f;
// SOURCE: notes R5-G1-8 1 (rolling resistance 10 x w / (velocity factor / 3.6) clamped to +-2).
inline constexpr float kRollingSpinScale = 10.0f;
inline constexpr float kRollingClamp = 2.0f;
// SOURCE: notes R5-G1-8 3 (low-speed auto brake below 1.0 m/s).
inline constexpr float kLowSpeedAutoBrakeSpeed = 1.0f;
// SOURCE: notes R5-G1-4 (h = 10 x dt).
inline constexpr float kMotionDampingRate = 10.0f;
// SOURCE: notes R5-G1-6 (gain 2, cap 6 x own load, same axle within 0.1 m).
inline constexpr float kAntiRollGain = 2.0f;
inline constexpr float kAntiRollCap = 6.0f;
inline constexpr float kAntiRollSameAxle = 0.1f;
// SOURCE: notes R6-G1-apply checker (base 1, +1 with wheels, +1 at or below 15 km/h, at most 3 substeps).
inline constexpr float kSubstepLowSpeedKmh = 15.0f;
inline constexpr uint32_t kMaxChassisSubsteps = 3;
inline constexpr float kHalf = 0.5f;
// SOURCE: notes R2-G1 A and R7-G1-standstill 1 (the vehicle step runs in BF3's 30 Hz tick).
inline constexpr float kChassisTickHz = 30.0f;
// SOURCE: notes R2-G1 B (the progressive multiplier's runtime setting, default 1).
inline constexpr float kProgressiveSetting = 1.0f;
// SOURCE: notes R2-G1 checker B (rebound damper force held to 0.8 x the spring force, one-sided).
inline constexpr float kDamperClamp = 0.8f;
// SOURCE: notes R7-G1-standstill (limits 1.0 m/s and 0.1 rad/s, k 150 and 10 1/s, pitch and roll share 0.1,
// at least min(n, 4) wheels, throttle 0.5), this constant and the six below it.
inline constexpr float kStandStillSpeedLimit = 1.0f;
inline constexpr float kStandStillAngularLimit = 0.1f;
inline constexpr float kStandStillLinearDamping = 150.0f;
inline constexpr float kStandStillAngularDamping = 10.0f;
inline constexpr float kStandStillTiltShare = 0.1f;
inline constexpr uint32_t kStandStillWheelMinimum = 4;
inline constexpr float kStandStillThrottle = 0.5f;

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

struct StandStillTest {
	float speed = 0, angular_speed = 0;
	uint32_t wheels = 0, grounded = 0;
	bool drive = false, started = false;
	float throttle = 0;
};

bool standing_still(const StandStillTest &p_test);

struct ChassisVelocity {
	std::array<float, 3> linear{}, angular{};
};

ChassisVelocity stand_still_damped(const ChassisVelocity &p_velocity, float p_dt);
}
