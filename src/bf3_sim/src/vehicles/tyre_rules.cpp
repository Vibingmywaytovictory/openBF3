#include "bf3/sim/vehicles/tyre_rules.h"

#include "bf3/sim/sim_types.h"
#include "bf3/sim/vehicles/land_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::vehicles {

namespace {

constexpr uint8_t kSubstepClockPerTick = 3;
constexpr uint8_t kSubstepClockSlow = 2;
constexpr uint8_t kSubstepClockFast = 3;
constexpr float kSubstepClockHz = 180.0f;

float sign_of(float p_value) {
	return p_value > 0.0f ? 1.0f : (p_value < 0.0f ? -1.0f : 0.0f);
}

float divided(float p_value, float p_divisor) {
	return std::fabs(p_divisor) < kTyreEpsilon ? p_value : p_value / p_divisor;
}

float curve_stiffness(float p_load_ratio, float p_pos_k) {
	return std::max(1.0f, (kCurveStiffnessBase - p_load_ratio * kCurveStiffnessLoad) * p_pos_k + kCurveStiffnessOffset);
}

}

float slip_divisor(float p_longitudinal_speed) {
	const float v = std::fabs(p_longitudinal_speed);
	return v > kSlipSlowSpeed ? v : std::max(kSlipSlowDivisor, kSlipDivisorFloor);
}

float slip_ratio_sample(float p_spin, float p_radius, float p_longitudinal_speed) {
	const float kappa = (p_spin * p_radius - p_longitudinal_speed) / slip_divisor(p_longitudinal_speed);
	return std::isfinite(kappa) ? std::clamp(kappa, -kSlipClamp, kSlipClamp) : kappa;
}

float slip_angle_sample(float p_lateral_speed, float p_longitudinal_speed) {
	return std::clamp(
			std::atan(p_lateral_speed / slip_divisor(p_longitudinal_speed)), -kSlipAngleClamp, kSlipAngleClamp);
}

SlipAverage slip_average_push(SlipAverage p_average, float p_sample, uint8_t p_capacity) {
	if (!std::isfinite(p_sample)) {
		return p_average;
	}
	if (p_average.count >= p_capacity && p_average.count > 0) {
		p_average.sum *= 1.0f - 1.0f / float(p_average.count);
		--p_average.count;
	}
	p_average.sum += p_sample;
	++p_average.count;
	return p_average;
}

float slip_average_value(SlipAverage p_average) {
	return p_average.sum / float(std::max<uint8_t>(p_average.count, 1));
}

GripStep tyre_grip(const GripRule &p_rule, TyreGrip p_state, float p_load, float p_slip_angle, float p_lateral_speed,
		float p_slip_ratio, float p_spin, float p_static_modifier, float p_dynamic_modifier) {
	const float grip = kFrictionSetting * kGripFriction * p_static_modifier;
	const float slide = kFrictionSetting * kSlideFriction * p_dynamic_modifier;
	const auto angle_limit = [&](float p_mu) {
		return p_rule.side_slip_angle_max_deg * core::kDegToRadF * p_load * p_mu * kSideSlipThresholdScale /
				kSlipThresholdMf;
	};
	const auto lateral_limit = [&](float p_mu) { return p_rule.wheel_velocity_x_min_deg * core::kDegToRadF * p_mu; };
	const auto ratio_limit = [&](float p_mu) {
		return p_rule.slip_ratio_max * p_load * p_mu * kSlipRatioThresholdScale / kSlipThresholdMf;
	};
	const float alpha = std::fabs(p_slip_angle), vy = std::fabs(p_lateral_speed), kappa = std::fabs(p_slip_ratio);
	if (p_state == TyreGrip::Slide) {
		if (alpha < angle_limit(slide) && vy < lateral_limit(slide) && kappa < ratio_limit(slide)) {
			return { TyreGrip::Grip, grip };
		}
		return { TyreGrip::Slide, slide };
	}
	if (p_state != TyreGrip::Grip) {
		return { TyreGrip::Slide, slide };
	}
	if (p_rule.always_grip || (p_spin > 0.0f && p_spin <= p_rule.angular_velocity_min_deg * core::kDegToRadF)) {
		return { TyreGrip::Grip, grip };
	}
	if (alpha > angle_limit(grip) || vy > lateral_limit(grip) || kappa > ratio_limit(grip)) {
		return { TyreGrip::Slide, slide };
	}
	return { TyreGrip::Grip, grip };
}

float friction_scale_at(std::span<const CurvePoint> p_points, float p_kmh) {
	if (p_points.empty()) {
		return 1.0f;
	}
	if (p_kmh <= p_points.front()[0]) {
		return p_points.front()[1];
	}
	for (size_t i = 1; i < p_points.size(); ++i) {
		if (p_kmh <= p_points[i][0]) {
			const float span = p_points[i][0] - p_points[i - 1][0];
			return span > 0.0f
					? p_points[i - 1][1] + (p_points[i][1] - p_points[i - 1][1]) * (p_kmh - p_points[i - 1][0]) / span
					: p_points[i][1];
		}
	}
	return p_points.back()[1];
}

float tyre_lateral_brake_scale(float p_brake, float p_longitudinal_speed, float p_brake_scale) {
	const float b = p_longitudinal_speed > kBrakeScaleSpeed ? p_brake : 0.0f;
	return 1.0f - b + b * p_brake_scale;
}

float tyre_load(float p_load, float p_mass, float p_constant_spring_force_friction_scale, float p_up_dot_normal,
		float p_planar_speed, float p_right_horizontal_length) {
	const float s =
			p_constant_spring_force_friction_scale != 0.0f ? p_mass * p_constant_spring_force_friction_scale : p_load;
	float k = 1.0f;
	if (p_planar_speed * p_planar_speed < kLowSpeedLoadFull * kLowSpeedLoadFull &&
			p_right_horizontal_length > kLowSpeedLoadLevel) {
		k = p_planar_speed <= kLowSpeedLoadStart ? kLowSpeedLoadFactor
												 : kLowSpeedLoadFactor +
						(1.0f - kLowSpeedLoadFactor) * (p_planar_speed - kLowSpeedLoadStart) /
								(kLowSpeedLoadFull - kLowSpeedLoadStart);
	}
	return k * s * p_up_dot_normal;
}

std::array<float, 4> tyre_peaks(float p_load_ratio) {
	std::array<float, 4> out{};
	for (size_t i = 0; i < 4; ++i) {
		out[i] = p_load_ratio < kLoadTableBreak
				? kLoadTableRow18[i] +
						(kLoadTableRow19[i] - kLoadTableRow18[i]) * (kLoadTableStep * p_load_ratio - kLoadTableRow)
				: kLoadTableRow19[i] + (p_load_ratio - kLoadTableBreak) * kLoadTableSlope * kLoadTableHighSlope[i];
	}
	return out;
}

float spin_term_scale(float p_stuck_seconds) {
	if (p_stuck_seconds <= kSpinTermFadeStart) {
		return kSpinTermFull;
	}
	return std::max((kSpinTermFadeEnd - p_stuck_seconds) * kSpinTermFadeRate, 0.0f);
}

TyreForces tyre_forces(const TyreCurveRule &p_rule, float p_slip_ratio, float p_slip_angle, float p_tyre_load,
		float p_mass, float p_longitudinal_scale, float p_lateral_scale, float p_planar_speed, float p_stuck_seconds) {
	if (!(p_tyre_load > 0.0f) || !(p_mass > 0.0f)) {
		return {};
	}
	const float load_ratio = kLoadTableScale * p_tyre_load / p_mass;
	const std::array<float, 4> peaks = tyre_peaks(load_ratio);
	const float a3 = peaks[0], a4 = peaks[1], a5 = peaks[2], a6 = peaks[3];
	const float kappa = std::fabs(p_slip_ratio);
	const float sx = -kappa / (1.0f + kappa);
	const float sy = std::tan(p_slip_angle) / (1.0f + kappa);
	const float a4n = -a4 / (1.0f + a4);
	const float a3n = a3 / (1.0f + a4);
	const float snx = divided(sx, a4n);
	const float sny = divided(sy, a3n);
	const float sigma = std::sqrt(snx * snx + sny * sny);
	const float x = sigma * a4n * sign_of(sx);
	const float kappa_e = x / (1.0f + x);
	const float alpha_e = std::atan(sigma * a3n * sign_of(sy));

	const float cy = curve_stiffness(load_ratio, p_rule.lateral_pos_k);
	const float cx = curve_stiffness(load_ratio, p_rule.longitudinal_pos_k);
	float neg = p_rule.longitudinal_neg_k;
	const float v2 = p_planar_speed * p_planar_speed;
	if (p_rule.allowed_to_spin && v2 < kSpinTermSpeedSquared) {
		neg += (1.0f - v2 / kSpinTermSpeedSquared) * spin_term_scale(p_stuck_seconds);
	}
	const float fy = (1.0f - std::exp(-std::fabs(cy * alpha_e)) - std::fabs(alpha_e) * p_rule.lateral_neg_k) * a5;
	const float fx = std::max(1.0f - std::exp(-std::fabs(cx * kappa_e)) - std::fabs(kappa_e) * neg, 0.0f) * a6 *
			(kappa_e < 0.0f ? -1.0f : 1.0f);
	const float fx0 = p_longitudinal_scale * p_tyre_load * fx;
	const float fy0 = p_lateral_scale * p_tyre_load * fy;

	float fxc = fx0, fyc = fy0;
	if (sigma >= kTyreEpsilon) {
		const float m = std::min(sigma, 1.0f);
		const float wy = sny / sigma, wx = snx / sigma;
		fxc = fx0 - (fx0 - fy0) * m * wy * wy;
		fyc = fy0 - (fy0 - fx0) * m * wx * wx;
	}
	const float q = std::sqrt(sx * sx + sy * sy);
	TyreForces out;
	if (q < kTyreEpsilon) {
		out.longitudinal = std::fabs(sx) * fxc;
		out.lateral = sy * fyc;
	} else {
		out.longitudinal = std::fabs(sx) / q * fxc;
		out.lateral = sy / q * fyc;
	}
	if (p_slip_ratio < 0.0f) {
		out.longitudinal = -out.longitudinal;
	}
	return out;
}

float stuck_seconds_step(float p_seconds, float p_speed_squared, float p_throttle, float p_dt) {
	return p_speed_squared < kStuckSpeedSquared && p_throttle > kStuckThrottle ? p_seconds + p_dt : 0.0f;
}

float tyre_low_speed_scale(float p_planar_speed, float p_drive_torque, float p_angular_speed) {
	return p_planar_speed < kLowSpeedForceSpeed && p_drive_torque == 0.0f && p_angular_speed < kLowSpeedForceSpin
			? p_planar_speed
			: 1.0f;
}

float tyre_force_clamp(float p_force, float p_max_factor, float p_mass) {
	const float limit = p_max_factor * p_mass;
	return std::clamp(p_force, -limit, limit);
}

float slope_grip_scale(float p_climb_up, float p_min_degrees, float p_max_degrees, int32_t p_exponent) {
	const float a = std::asin(std::clamp(p_climb_up, 0.0f, 1.0f));
	const float hi = p_max_degrees * core::kDegToRadF;
	const float lo = std::min(p_min_degrees * core::kDegToRadF, hi - core::kDegToRadF);
	const float r = std::clamp((std::clamp(a, lo, hi) - lo) / (hi - lo), 0.0f, 1.0f);
	float scale = 1.0f - r;
	for (int32_t i = 1; i < p_exponent; ++i) {
		scale *= 1.0f - r;
	}
	return scale;
}

float wheel_reaction_torque(float p_longitudinal_force, float p_forward_up, float p_mass, uint32_t p_wheels,
		float p_radius, float p_radius_multiplier, float p_diff_gear_ratio) {
	const float slope = -p_forward_up * p_mass * kChassisGravity / float(std::max<uint32_t>(p_wheels, 1u));
	return (p_longitudinal_force - slope) * p_radius * p_radius_multiplier / (kReactionDivisor * p_diff_gear_ratio);
}

bool wheel_spin_gate(bool p_any_drive, float p_planar_speed) {
	return p_any_drive || p_planar_speed > kSpinGateSpeed;
}

float wheel_spin_step(float p_spin, float p_torque, float p_dt, float p_inertia) {
	return p_spin + p_torque * p_dt / p_inertia;
}

uint32_t land_substeps_due(uint8_t &r_clock, float p_speed) {
	const uint8_t cost = chassis_substeps(p_speed, true) == kMaxChassisSubsteps ? kSubstepClockSlow : kSubstepClockFast;
	const uint32_t clock = uint32_t(r_clock % kSubstepClockFast) + kSubstepClockPerTick;
	const uint32_t due = clock / cost;
	r_clock = uint8_t(clock - due * cost);
	return due;
}

float land_substep_seconds(float p_speed) {
	return (chassis_substeps(p_speed, true) == kMaxChassisSubsteps ? float(kSubstepClockSlow)
																   : float(kSubstepClockFast)) /
			kSubstepClockHz;
}

}
