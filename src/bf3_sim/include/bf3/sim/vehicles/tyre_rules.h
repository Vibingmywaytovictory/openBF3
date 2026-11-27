#pragma once

#include "bf3/core/math.h"
#include "bf3/sim/vehicles/chassis_rules.h"

#include <array>
#include <cstdint>
#include <span>

namespace bf3::sim::vehicles {

// SOURCE: notes R6-G1-tyre 1 (slow speed 7.2 km/h, slow divisor 10 m/s floored at 0.001, angle clamp pi,
// buffers of 5 and 3 samples).
inline constexpr float kSlipSlowSpeed = 7.2f / 3.6f;
inline constexpr float kSlipSlowDivisor = 10.0f;
inline constexpr float kSlipDivisorFloor = 0.001f;
inline constexpr float kSlipAngleClamp = 3.14159265358979323846f;
inline constexpr uint8_t kSlipRatioSamples = 5;
inline constexpr uint8_t kSlipAngleSamples = 3;
// SOURCE: notes R6-G1-tyre 2 (friction setting 0.9, grip 2.5, slide 1.875, threshold divisor 20000, factors 10 and 2).
inline constexpr float kFrictionSetting = 0.9f;
inline constexpr float kGripFriction = 2.5f;
inline constexpr float kSlideFriction = 1.875f;
inline constexpr float kSlipThresholdMf = 20000.0f;
inline constexpr float kSideSlipThresholdScale = 10.0f;
inline constexpr float kSlipRatioThresholdScale = 2.0f;
// SOURCE: notes R6-G1-tyre 4 (load factor 0.25 up to 1 m/s, rising to 1 at 3 m/s, roll level test 0.99).
inline constexpr float kLowSpeedLoadFactor = 0.25f;
inline constexpr float kLowSpeedLoadLevel = 0.99f;
inline constexpr float kLowSpeedLoadFull = 3.0f;
inline constexpr float kLowSpeedLoadStart = 1.0f;
// SOURCE: notes R6-G1-tyre 5 (L = 1200 Fz / M, break 9500, rows 18 and 19, the high-load slopes).
inline constexpr float kLoadTableScale = 1200.0f;
inline constexpr float kLoadTableBreak = 9500.0f;
inline constexpr float kLoadTableStep = 0.002f;
inline constexpr float kLoadTableRow = 18.0f;
inline constexpr float kLoadTableSlope = 0.0002f;
inline constexpr std::array<float, 4> kLoadTableRow18{ 0.55676f, 0.416691f, 1.036957f, 1.101291f };
inline constexpr std::array<float, 4> kLoadTableRow19{ 0.569186f, 0.425695f, 1.037847f, 1.103871f };
inline constexpr std::array<float, 4> kLoadTableHighSlope{ 0.09402996f, 0.06829602f, 0.00666904f, 0.01912701f };
// SOURCE: notes R6-G1-tyre 7 (the curve stiffness c).
inline constexpr float kCurveStiffnessBase = 1.75f;
inline constexpr float kCurveStiffnessLoad = 0.0001f;
inline constexpr float kCurveStiffnessOffset = 5.0f;
// SOURCE: notes R6-G1-tyre 6 and 8 (divisions skipped below 1.19e-7).
inline constexpr float kTyreEpsilon = 1.19e-7f;
// SOURCE: notes R6-G1-tyre 7 and its checker (IsAllowedToSpin term; stuck timer below 8 m/s with input over 0.5).
inline constexpr float kSpinTermFull = 1.4f;
inline constexpr float kSpinTermFadeStart = 2.0f;
inline constexpr float kSpinTermFadeEnd = 4.0f;
inline constexpr float kSpinTermFadeRate = 0.7f;
inline constexpr float kSpinTermSpeedSquared = 100.0f;
inline constexpr float kStuckSpeedSquared = 64.0f;
inline constexpr float kStuckThrottle = 0.5f;
// SOURCE: notes R6-G1-tyre 3 (brake blend above 3.0 m/s).
inline constexpr float kBrakeScaleSpeed = 3.0f;
// SOURCE: notes R6-G1-apply 1, 2c and 4 (low-speed scaling 1 m/s and 0.1 rad/s, 1e-6 axis guard, reaction 16,
// torque gate 0.01 and 0.01 m/s).
inline constexpr float kLowSpeedForceSpeed = 1.0f;
inline constexpr float kLowSpeedForceSpin = 0.1f;
inline constexpr float kAxisEpsilon = 1.0e-6f;
inline constexpr float kReactionDivisor = 16.0f;
inline constexpr float kSpinGateTorque = 0.01f;
inline constexpr float kSpinGateSpeed = 0.01f;

enum class TyreGrip : uint8_t { Reset = 0, Grip = 1, Slide = 2 };

struct SlipAverage {
	float sum = 0;
	uint8_t count = 0;
};

float slip_divisor(float p_longitudinal_speed);

float slip_ratio_sample(float p_spin, float p_radius, float p_longitudinal_speed);

float slip_angle_sample(float p_lateral_speed, float p_longitudinal_speed);

SlipAverage slip_average_push(SlipAverage p_average, float p_sample, uint8_t p_capacity);

float slip_average_value(SlipAverage p_average);

struct GripRule {
	float angular_velocity_min_deg = 0;
	float side_slip_angle_max_deg = 0;
	float wheel_velocity_x_min_deg = 0;
	float slip_ratio_max = 0;
	bool always_grip = false;
};

struct GripStep {
	TyreGrip state = TyreGrip::Reset;
	float friction = 0;
};

GripStep tyre_grip(const GripRule &p_rule, TyreGrip p_state, float p_load, float p_slip_angle, float p_lateral_speed,
		float p_slip_ratio, float p_spin, float p_static_modifier, float p_dynamic_modifier);

float friction_scale_at(std::span<const CurvePoint> p_points, float p_kmh);

float tyre_lateral_brake_scale(float p_brake, float p_longitudinal_speed, float p_brake_scale);

float tyre_load(float p_load, float p_mass, float p_constant_spring_force_friction_scale, float p_up_dot_normal,
		float p_planar_speed, float p_right_horizontal_length);

std::array<float, 4> tyre_peaks(float p_load_ratio);

struct TyreCurveRule {
	float lateral_pos_k = 0, lateral_neg_k = 0;
	float longitudinal_pos_k = 0, longitudinal_neg_k = 0;
	bool allowed_to_spin = false;
};

struct TyreForces {
	float longitudinal = 0;
	float lateral = 0;
};

float spin_term_scale(float p_stuck_seconds);

TyreForces tyre_forces(const TyreCurveRule &p_rule, float p_slip_ratio, float p_slip_angle, float p_tyre_load,
		float p_mass, float p_longitudinal_scale, float p_lateral_scale, float p_planar_speed, float p_stuck_seconds);

float stuck_seconds_step(float p_seconds, float p_speed_squared, float p_throttle, float p_dt);

float tyre_low_speed_scale(float p_planar_speed, float p_drive_torque, float p_angular_speed);

float tyre_force_clamp(float p_force, float p_max_factor, float p_mass);

float slope_grip_scale(float p_climb_up, float p_min_degrees, float p_max_degrees, int32_t p_exponent);

float wheel_reaction_torque(float p_longitudinal_force, float p_forward_up, float p_mass, uint32_t p_wheels,
		float p_radius, float p_radius_multiplier, float p_diff_gear_ratio);

bool wheel_spin_gate(bool p_any_drive, float p_planar_speed);

float wheel_spin_step(float p_spin, float p_torque, float p_dt, float p_inertia);

uint32_t land_substeps_due(uint8_t &r_clock, float p_speed);

float land_substep_seconds(float p_speed);

}
