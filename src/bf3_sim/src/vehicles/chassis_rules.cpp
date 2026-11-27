#include "bf3/sim/vehicles/chassis_rules.h"

#include "bf3/sim/vehicles/land_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::vehicles {

namespace {

float segment_at(const CurvePoint &p_a, const CurvePoint &p_b, float p_key) {
	const float span = p_b[0] - p_a[0];
	if (!(std::fabs(span) > 0.0f)) {
		return p_b[1];
	}
	return p_a[1] + (p_b[1] - p_a[1]) * (p_key - p_a[0]) / span;
}

float curve_inside(std::span<const CurvePoint> p_points, float p_key) {
	for (size_t i = 1; i < p_points.size(); ++i) {
		if (p_key <= p_points[i][0]) {
			return segment_at(p_points[i - 1], p_points[i], p_key);
		}
	}
	return segment_at(p_points[p_points.size() - 2], p_points.back(), p_key);
}

}

float steer_curve_at(std::span<const CurvePoint> p_points, float p_key) {
	if (p_points.empty()) {
		return 1.0f;
	}
	if (p_points.size() == 1 || p_key <= p_points.front()[0]) {
		return p_points.front()[1];
	}
	return curve_inside(p_points, p_key);
}

float loss_curve_at(std::span<const CurvePoint> p_points, float p_key) {
	if (p_points.empty()) {
		return 0.0f;
	}
	if (p_points.size() == 1) {
		return p_points.front()[1];
	}
	if (p_key <= p_points.front()[0]) {
		return segment_at(p_points[0], p_points[1], p_key);
	}
	return curve_inside(p_points, p_key);
}

uint32_t chassis_substeps(float p_speed, bool p_wheeled) {
	uint32_t n = 1;
	if (p_wheeled) {
		++n;
	}
	if (std::fabs(p_speed) * kKmhPerMs <= kSubstepLowSpeedKmh) {
		++n;
	}
	return std::min(n, kMaxChassisSubsteps);
}

float spring_constant(float p_mass, uint32_t p_wheels, float p_stiffness, float p_length) {
	const float rest = float(std::max<uint32_t>(p_wheels, 1u)) * (1.0f - p_stiffness) * p_length;
	return rest > 0.0f ? p_mass * kChassisGravity / rest : 0.0f;
}

float damper_coefficient(float p_mass, uint32_t p_wheels, float p_damping, float p_progressive, float p_substep_dt) {
	return p_damping * p_mass / float(std::max<uint32_t>(p_wheels, 1u)) * p_progressive / p_substep_dt;
}

float damper_clamped_coefficient(float p_coefficient, float p_spring_force, float p_extension_speed) {
	if (!(p_spring_force > 0.0f) || !(p_extension_speed > 0.0f)) {
		return p_coefficient;
	}
	return std::min(p_coefficient, kDamperClamp * p_spring_force / p_extension_speed);
}

float steer_sensitivity(std::span<const CurvePoint> p_points, float p_speed, bool p_inverted) {
	const float sens = std::clamp(
			steer_curve_at(p_points, std::fabs(p_speed) * kKmhPerMs), -kSteerSensitivityClamp, kSteerSensitivityClamp);
	return p_inverted ? sens : -sens;
}

float steer_angle(float p_yaw, float p_sensitivity) {
	return -kSteerHalfTurn * p_yaw * p_sensitivity;
}

float transmission_loss_resistance(
		std::span<const CurvePoint> p_points, float p_planar_speed, float p_strength, float p_mass, uint32_t p_wheels) {
	if (p_points.empty()) {
		return 0.0f;
	}
	const float key = kKmhPerMs * std::fabs(p_planar_speed) + kLossKmhPerStrength * (1.0f - p_strength);
	const float loss = loss_curve_at(p_points, key);
	return loss > 0.0f ? p_mass * loss / float(std::max<uint32_t>(p_wheels, 1u)) / kLossDivisor : 0.0f;
}

float engine_brake_resistance(float p_mass, float p_speed) {
	const float v = std::fabs(p_speed);
	const float fade = v <= kEngineBrakeFadeSpeed ? kEngineBrakeFadeSpeed + 1.0f - v : 1.0f;
	return p_mass * fade * kEngineBrakeScale * std::max(v, 1.0f) * kEngineBrakeFraction;
}

float rolling_resistance(float p_spin, float p_base_factor, float p_velocity_factor_kmh, float p_width,
		float p_wheel_mass, uint32_t p_wheels) {
	if (!(p_velocity_factor_kmh > 0.0f)) {
		return 0.0f;
	}
	const float r =
			std::clamp(kRollingSpinScale * p_spin / (p_velocity_factor_kmh / kKmhPerMs), -kRollingClamp, kRollingClamp);
	return p_base_factor * r * p_width * p_wheel_mass / float(std::max<uint32_t>(p_wheels, 1u));
}

float spin_after_resistance(float p_spin, float p_resistance, float p_dt, float p_inertia) {
	const float delta = p_resistance * p_dt / p_inertia;
	if (p_spin * delta < 0.0f && std::fabs(delta) > std::fabs(p_spin)) {
		return 0.0f;
	}
	return p_spin + delta;
}

std::array<float, 3> motion_damping_angular(const MotionDampingRule &p_rule, float p_dt) {
	const float h = kMotionDampingRate * p_dt;
	return { std::exp(-h * p_rule.pitch), std::exp(-h * p_rule.yaw), std::exp(-h * p_rule.roll) };
}

std::array<float, 3> motion_damping_linear(const MotionDampingRule &p_rule, float p_dt) {
	const float h = kMotionDampingRate * p_dt * p_rule.linear;
	return { std::exp(-h * p_rule.linear_modifier[0]), std::exp(-h * p_rule.linear_modifier[1]),
		std::exp(-h * p_rule.linear_modifier[2]) };
}

float wind_drag(const WindRule &p_rule, float p_speed) {
	const float reference = p_rule.velocity_factor_kmh / kKmhPerMs;
	const float v2 = p_speed * p_speed;
	const float k = std::clamp(reference > 0.0f ? v2 / (reference * reference) : p_rule.velocity_factor_max,
			p_rule.velocity_factor_min, p_rule.velocity_factor_max);
	const float c = kHalf * p_rule.coefficient_of_air_friction * p_rule.air_drag_area * p_rule.air_density;
	return v2 * c * k * p_rule.base_factor;
}

float down_force(float p_speed, float p_air_density, float p_base_factor, bool p_use_wheel_factor, float p_wheel_factor,
		uint32_t p_wheels, uint32_t p_grounded) {
	float f = std::fabs(p_speed) * p_air_density * p_base_factor;
	if (p_use_wheel_factor && p_grounded < p_wheels) {
		f *= float(p_wheels - p_grounded) * p_wheel_factor;
	}
	return f;
}

std::array<float, 2> anti_roll_loads(float p_load_a, float p_load_b, float p_stiffness) {
	const float avg = kHalf * (p_load_a + p_load_b);
	const auto bar = [&](float p_load) {
		return std::min(std::max(p_load + kAntiRollGain * p_stiffness * (p_load - avg), 0.0f), kAntiRollCap * p_load);
	};
	return { bar(p_load_a), bar(p_load_b) };
}

bool standing_still(const StandStillTest &p_test) {
	return !p_test.drive && !(p_test.started && p_test.throttle > kStandStillThrottle) &&
			p_test.grounded >= std::min(p_test.wheels, kStandStillWheelMinimum) &&
			p_test.speed < kStandStillSpeedLimit && p_test.angular_speed < kStandStillAngularLimit;
}

ChassisVelocity stand_still_damped(const ChassisVelocity &p_velocity, float p_dt) {
	const float linear = std::exp(-kStandStillLinearDamping * p_dt);
	const float yaw = std::exp(-kStandStillAngularDamping * p_dt);
	const float tilt = std::exp(-kStandStillTiltShare * kStandStillAngularDamping * p_dt);
	const std::array<float, 3> &v = p_velocity.linear;
	const std::array<float, 3> &w = p_velocity.angular;
	return { { v[0] * linear, v[1], v[2] * linear }, { w[0] * tilt, w[1] * yaw, w[2] * tilt } };
}
}
