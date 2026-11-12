#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace bf3::sim::vehicles {

inline constexpr float kTurretWrapMargin = 0.017453292f;
inline constexpr float kTurretMinDirectRotation = 1e-4f;
inline constexpr float kInputCurveFloor = 1e-6f;
inline constexpr float kPositionLockInputFloor = 1e-6f;
inline constexpr int32_t kPositionLockWaitTicks = 5;
inline constexpr float kPositionLockDistance = 2000.0f;

struct ExtendedLimitsRule {
	bool present = false;
	float heading = 0;
	float width = 0;
	float falloff = 0;
	float min_degrees = 0;
	float max_degrees = 0;
};

struct RotationBodyRule {
	float mass = 1;
	float inertia = 1;
	float momentum_multiplier = 1;
	float force_modifier = 0;
	float reset_force_modifier = 0;
	float reset_input_threshold = 0;
	float momentum_threshold = 0;
	float damping_acc = 1;
	float damping_deacc = 1;
	bool use_linear_input = true;
	bool use_threshold = false;
	bool use_damping = false;
	bool post_satisfy = false;
	bool constrained = false;
	float min_degrees = 0;
	float max_degrees = 0;
	ExtendedLimitsRule extended;
	float world_lock_efficiency = 0;
};

struct RotationBodyState {
	float angle = 0;
	float momentum = 0;

	bool operator==(const RotationBodyState &) const = default;
};

struct RotationLimits {
	float min = 0;
	float max = 0;
};

float rotation_input_torque(const RotationBodyRule &p_rule, float p_level, float p_angle);

float alignment_input(float p_error, float p_speed);

bool alignment_done(float p_error, float p_stop_tolerance_degrees);

RotationLimits rotation_limits(const RotationBodyRule &p_rule, float p_parent_degrees, bool p_has_parent);

void rotation_body_step(const RotationBodyRule &p_rule, RotationBodyState &r_state, float p_torque,
		const RotationLimits &p_limits, float p_dt);

float wrap_angle(float p_angle);

float rotation_direct(const RotationBodyRule &p_rule, float p_angle, float p_rotation, const RotationLimits &p_limits);

float world_lock_rotation(float p_efficiency, float p_parent_rotation);

float rotation_steady_rate(const RotationBodyRule &p_rule, float p_level, float p_dt);

bool position_lock_tick(int32_t &r_counter, float p_level);

using InputCurvePoint = std::array<float, 2>;

float input_curve(std::span<const InputCurvePoint> p_points, float p_magnitude);

void shape_curve_inputs(std::span<const InputCurvePoint> p_points, std::span<float> r_levels, bool p_square);

}
