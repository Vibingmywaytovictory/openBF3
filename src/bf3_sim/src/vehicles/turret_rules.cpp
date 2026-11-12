#include "bf3/sim/vehicles/turret_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::vehicles {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kRadPerDegree = kPi / 180.0f;
constexpr int kSteadyRateTicks = 512;

void damp(const RotationBodyRule &p_rule, bool p_accelerating, float &r_momentum) {
	if (p_rule.use_damping) {
		r_momentum *= p_accelerating ? p_rule.damping_acc : p_rule.damping_deacc;
	}
}

void clamp_threshold(const RotationBodyRule &p_rule, float &r_momentum) {
	if (p_rule.use_threshold) {
		r_momentum = std::clamp(r_momentum, -p_rule.momentum_threshold, p_rule.momentum_threshold);
	}
}

float rotation_torque(const RotationBodyRule &p_rule, float p_shaped) {
	return p_rule.mass > 0.0f ? p_shaped * p_rule.force_modifier * p_rule.momentum_multiplier / p_rule.mass : 0.0f;
}

float shaped_level(const RotationBodyRule &p_rule, float p_level) {
	return p_rule.use_linear_input ? p_level : p_level * std::fabs(p_level);
}

}

float rotation_input_torque(const RotationBodyRule &p_rule, float p_level, float p_angle) {
	const float shaped = shaped_level(p_rule, p_level);
	float torque = -rotation_torque(p_rule, shaped);
	if (std::fabs(shaped) < p_rule.reset_input_threshold) {
		torque += p_rule.mass > 0.0f ? p_angle * p_rule.reset_force_modifier * p_rule.momentum_multiplier / p_rule.mass
									 : 0.0f;
	}
	return torque;
}

float alignment_input(float p_error, float p_speed) {
	return std::clamp(p_speed * p_error, -1.0f, 1.0f);
}

bool alignment_done(float p_error, float p_stop_tolerance_degrees) {
	return std::fabs(p_error) < p_stop_tolerance_degrees * kRadPerDegree;
}

RotationLimits rotation_limits(const RotationBodyRule &p_rule, float p_parent_degrees, bool p_has_parent) {
	float min = p_rule.min_degrees;
	float max = p_rule.max_degrees;
	const ExtendedLimitsRule &e = p_rule.extended;
	if (e.present && p_has_parent) {
		const float d = std::fabs(p_parent_degrees - e.heading) - (e.width - e.falloff) / 2.0f;
		if (d <= 0.0f) {
			min = e.min_degrees;
			max = e.max_degrees;
		} else if (d < e.falloff) {
			min = e.min_degrees + (min - e.min_degrees) * d / e.falloff;
			max = e.max_degrees + (max - e.max_degrees) * d / e.falloff;
		}
	}
	return { std::clamp(min * kRadPerDegree, -kPi, 0.0f), std::clamp(max * kRadPerDegree, 0.0f, kPi) };
}

float wrap_angle(float p_angle) {
	float a = std::fmod(p_angle, kTwoPi);
	if (a <= -kPi) {
		a += kTwoPi;
	} else if (a > kPi) {
		a -= kTwoPi;
	}
	return a;
}

void rotation_body_step(const RotationBodyRule &p_rule, RotationBodyState &r_state, float p_torque,
		const RotationLimits &p_limits, float p_dt) {
	float momentum = r_state.momentum;
	const bool accelerating = p_torque * momentum > 0.0f && std::fabs(p_torque) >= std::fabs(momentum);
	if (!p_rule.post_satisfy) {
		damp(p_rule, accelerating, momentum);
		clamp_threshold(p_rule, momentum);
	}
	const float old_angle = r_state.angle;
	const float inertia = p_rule.inertia > 0.0f ? p_rule.inertia : 1.0f;
	const float mid = momentum + p_torque * p_dt / 2.0f;
	float angle = wrap_angle(old_angle + p_dt * mid / inertia);
	momentum += p_torque * p_dt;
	if (p_rule.post_satisfy) {
		clamp_threshold(p_rule, momentum);
		damp(p_rule, accelerating, momentum);
	}
	if (p_rule.constrained && angle != old_angle) {
		const float w = momentum / inertia;
		if (w < 0.0f) {
			const float t = angle - old_angle > kTurretWrapMargin ? angle - kTwoPi : angle;
			if (t < p_limits.min) {
				angle = p_limits.min;
				momentum = 0.0f;
			}
		} else if (w > 0.0f) {
			const float t = angle - old_angle < -kTurretWrapMargin ? angle + kTwoPi : angle;
			if (t > p_limits.max) {
				angle = p_limits.max;
				momentum = 0.0f;
			}
		}
	}
	r_state.angle = angle;
	r_state.momentum = momentum;
}

float rotation_direct(const RotationBodyRule &p_rule, float p_angle, float p_rotation, const RotationLimits &p_limits) {
	if (std::fabs(p_rotation) <= kTurretMinDirectRotation) {
		return p_angle;
	}
	if (!p_rule.constrained) {
		return wrap_angle(p_angle + p_rotation);
	}
	const float from = std::clamp(p_angle, p_limits.min, p_limits.max);
	return wrap_angle(std::clamp(from + p_rotation, p_limits.min, p_limits.max));
}

float world_lock_rotation(float p_efficiency, float p_parent_rotation) {
	return p_efficiency > 0.0f ? -p_parent_rotation * p_efficiency : 0.0f;
}

float rotation_steady_rate(const RotationBodyRule &p_rule, float p_level, float p_dt) {
	const float inertia = p_rule.inertia > 0.0f ? p_rule.inertia : 1.0f;
	const float torque = rotation_torque(p_rule, shaped_level(p_rule, std::fabs(p_level)));
	float momentum = 0.0f;
	float mid = 0.0f;
	for (int i = 0; i < kSteadyRateTicks; ++i) {
		const bool accelerating = torque * momentum > 0.0f && std::fabs(torque) >= std::fabs(momentum);
		if (!p_rule.post_satisfy) {
			damp(p_rule, accelerating, momentum);
			clamp_threshold(p_rule, momentum);
		}
		mid = momentum + torque * p_dt / 2.0f;
		momentum += torque * p_dt;
		if (p_rule.post_satisfy) {
			clamp_threshold(p_rule, momentum);
			damp(p_rule, accelerating, momentum);
		}
	}
	return mid / inertia;
}

bool position_lock_tick(int32_t &r_counter, float p_level) {
	if (r_counter > 0) {
		--r_counter;
	}
	if (std::fabs(p_level) > kPositionLockInputFloor) {
		r_counter = kPositionLockWaitTicks;
		return false;
	}
	if (r_counter == 0) {
		r_counter = -1;
		return true;
	}
	return false;
}

float input_curve(std::span<const InputCurvePoint> p_points, float p_magnitude) {
	const float m = std::fabs(p_magnitude);
	if (p_points.size() < 2 || m < kInputCurveFloor) {
		return p_magnitude;
	}
	size_t i = 1;
	while (i + 1 < p_points.size() && m > p_points[i][0]) {
		++i;
	}
	const InputCurvePoint &a = p_points[i - 1];
	const InputCurvePoint &b = p_points[i];
	const float x = std::min(m, p_points.back()[0]);
	const float span = b[0] - a[0];
	const float f = span > 0.0f ? a[1] + (b[1] - a[1]) * (x - a[0]) / span : b[1];
	return p_magnitude < 0.0f ? -f : f;
}

void shape_curve_inputs(std::span<const InputCurvePoint> p_points, std::span<float> r_levels, bool p_square) {
	float m = 0.0f;
	for (const float l : r_levels) {
		m = p_square ? std::max(m, std::fabs(l)) : m + l * l;
	}
	if (!p_square) {
		m = std::sqrt(m);
	}
	m = std::min(m, 1.0f);
	if (m < kInputCurveFloor) {
		return;
	}
	const float scale = input_curve(p_points, m) / m;
	for (float &l : r_levels) {
		l = std::clamp(l * scale, -1.0f, 1.0f);
	}
}

}
