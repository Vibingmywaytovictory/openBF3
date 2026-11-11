#include "bf3/sim/vehicles/land_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::vehicles {

namespace {

float sign_of(float p_value) {
	return p_value > 0.0f ? 1.0f : (p_value < 0.0f ? -1.0f : 0.0f);
}

bool shifting(GearPhase p_phase) {
	return p_phase == GearPhase::Disengage || p_phase == GearPhase::Wait || p_phase == GearPhase::Reengage;
}

float speed_at(std::span<const float> p_speeds, int32_t p_one_based) {
	if (p_one_based < 1 || size_t(p_one_based) > p_speeds.size()) {
		return INFINITY;
	}
	return p_speeds[size_t(p_one_based) - 1] / kKmhPerMs;
}

}

float shape_deadzone(float p_input, float p_deadzone, bool p_signed) {
	const float dz = std::max(p_deadzone - kDeadzoneReduction, 0.0f);
	const auto shaped = [dz](float p_x) { return p_x <= dz || dz >= 1.0f ? 0.0f : (p_x - dz) / (1.0f - dz); };
	if (!p_signed) {
		return p_input <= 0.0f ? 0.0f : shaped(p_input);
	}
	return sign_of(p_input) * shaped(std::fabs(p_input));
}

float shape_inertia(float p_previous, float p_target, float p_dt, const InputAxisRule &p_axis) {
	if ((p_axis.inertia_in <= 0.0f && p_axis.inertia_out <= 0.0f) || p_target == p_previous) {
		return p_target;
	}
	const float s = p_previous > p_target ? p_previous : -p_previous;
	const float a = p_axis.inertia_in > 0.0f ? p_dt / p_axis.inertia_in : kInstantInertiaStep;
	const float b = p_axis.inertia_out > 0.0f ? p_dt / p_axis.inertia_out : kInstantInertiaStep;
	const float m = p_axis.inertia_min_ratio;
	const float beyond = std::max(s - m, 0.0f);
	float step = 0.0f;
	if (s <= -m) {
		step = b;
	} else if (beyond >= a) {
		step = a;
	} else {
		step = s + m + std::max(b - beyond, 0.0f);
	}
	const float distance = p_target - p_previous;
	return p_previous + sign_of(distance) * std::min(std::fabs(distance), step);
}

float torque_curve_at(float p_rpm, std::span<const float> p_rpm_points, std::span<const float> p_torque_points) {
	const size_t n = std::min(p_rpm_points.size(), p_torque_points.size());
	if (n == 0) {
		return 0.0f;
	}
	if (n == 1) {
		return p_torque_points[0];
	}
	size_t above = 0;
	while (above < n && !(p_rpm_points[above] > p_rpm)) {
		++above;
	}
	if (above == 0) {
		return p_torque_points[0];
	}
	const size_t hi = above == n ? n - 1 : above;
	const size_t lo = hi - 1;
	const float span = p_rpm_points[hi] - p_rpm_points[lo];
	if (!(span != 0.0f)) {
		return p_torque_points[lo];
	}
	return p_torque_points[lo] + (p_torque_points[hi] - p_torque_points[lo]) * (p_rpm - p_rpm_points[lo]) / span;
}

float engine_torque(float p_rpm, std::span<const float> p_rpm_points, std::span<const float> p_torque_points,
		float p_power_multiplier, float p_throttle, float p_clutch) {
	return (1.0f - p_clutch) * p_throttle * torque_curve_at(p_rpm, p_rpm_points, p_torque_points) * p_power_multiplier;
}

int32_t gear_step(const GearboxRule &p_rule, int32_t p_gear, int32_t p_delta) {
	int32_t target = p_gear + p_delta;
	if (!p_rule.use_neutral_gear && target == 0) {
		target = p_delta > 0 ? 1 : -1;
	}
	return std::clamp(target, -p_rule.reverse_gears, p_rule.forward_gears);
}

GearRequest gearbox_request(
		const GearboxRule &p_rule, const GearboxState &p_state, float p_throttle, float p_brake, float p_speed) {
	if (p_rule.gearbox_type != 1 || p_state.phase != GearPhase::Idle) {
		return GearRequest::None;
	}
	const int32_t g = p_state.gear;
	const bool throttle = p_throttle > kShiftInputThreshold;
	const bool brake = p_brake > kShiftInputThreshold;
	const bool brake_turn = brake && p_speed < p_rule.opposite_dir_gear_change_max_speed / kKmhPerMs;
	if (g == 0) {
		if (brake) {
			return GearRequest::OppositeDirection;
		}
		if (throttle) {
			return p_state.reverse_direction != 0 ? GearRequest::Down : GearRequest::Up;
		}
		return GearRequest::None;
	}
	const bool forward = g > 0;
	const std::span<const float> speeds = forward ? p_rule.forward_speeds : p_rule.reverse_speeds;
	const int32_t index = forward ? g : -g;
	const GearRequest higher = forward ? GearRequest::Up : GearRequest::Down;
	const GearRequest lower = forward ? GearRequest::Down : GearRequest::Up;
	GearRequest out = GearRequest::None;
	if (throttle && p_speed > speed_at(speeds, index)) {
		out = higher;
	} else if (index >= 2 && p_speed < speed_at(speeds, index - 1) * p_rule.gear_down_speed_factor) {
		out = lower;
	} else if (index == 1 && !throttle && p_speed < kStopSpeed) {
		out = lower;
	}
	if (brake_turn) {
		out = lower;
	}
	return out;
}

bool gearbox_accept(const GearboxRule &p_rule, GearboxState &r_state, GearRequest p_request) {
	if (p_request == GearRequest::None || r_state.phase != GearPhase::Idle) {
		return false;
	}
	if (p_request == GearRequest::OppositeDirection) {
		r_state.phase = GearPhase::OppositeDirection;
		r_state.timer = p_rule.opposite_dir_gear_change_time;
		return true;
	}
	const int8_t delta = p_request == GearRequest::Up ? int8_t(1) : int8_t(-1);
	if (gear_step(p_rule, r_state.gear, delta) == r_state.gear) {
		return false;
	}
	r_state.pending = delta;
	r_state.timer = p_rule.gear_change_time;
	r_state.phase = p_rule.gearbox_mode == 1 ? GearPhase::Disengage : GearPhase::Wait;
	return true;
}

void gearbox_advance(const GearboxRule &p_rule, GearboxState &r_state, float p_dt) {
	const float ramp = p_rule.clutch_speed_factor * p_dt;
	switch (r_state.phase) {
		case GearPhase::Idle:
			return;
		case GearPhase::Disengage:
			r_state.clutch += ramp;
			if (r_state.clutch >= 1.0f || !(ramp > 0.0f)) {
				r_state.clutch = 1.0f;
				r_state.timer = p_rule.gear_change_time;
				r_state.phase = GearPhase::Wait;
			}
			return;
		case GearPhase::Wait:
			r_state.timer -= p_dt;
			if (r_state.timer <= 0.0f) {
				r_state.gear = gear_step(p_rule, r_state.gear, r_state.pending);
				r_state.pending = 0;
				r_state.timer = 0.0f;
				r_state.phase = p_rule.gearbox_mode == 1 ? GearPhase::Reengage : GearPhase::Idle;
			}
			return;
		case GearPhase::Reengage:
			r_state.clutch -= ramp;
			if (r_state.clutch <= 0.0f || !(ramp > 0.0f)) {
				r_state.clutch = 0.0f;
				r_state.phase = GearPhase::Idle;
			}
			return;
		case GearPhase::OppositeDirection:
			r_state.timer -= p_dt;
			if (r_state.timer <= 0.0f) {
				r_state.timer = 0.0f;
				r_state.reverse_direction = r_state.reverse_direction != 0 ? 0 : 1;
				r_state.phase = GearPhase::Idle;
			}
			return;
	}
}

bool gearbox_clutch_pressed(const GearboxRule &p_rule, const GearboxState &p_state) {
	if (p_rule.gearbox_mode == 0) {
		return p_state.phase == GearPhase::Wait;
	}
	return p_rule.gearbox_mode == 1 && p_rule.use_classic_auto_clutch && shifting(p_state.phase);
}

float wheel_rpm(std::span<const float> p_wheel_angular_velocities, float p_gear_ratio, float p_diff_gear_ratio,
		float p_transmission_efficiency) {
	if (p_wheel_angular_velocities.empty()) {
		return 0.0f;
	}
	float sum = 0.0f;
	for (const float w : p_wheel_angular_velocities) {
		sum += std::fabs(w) * kRadPerSecToRpm * std::fabs(p_gear_ratio) * p_diff_gear_ratio * p_transmission_efficiency;
	}
	return sum / float(p_wheel_angular_velocities.size());
}

float engine_rpm_target(const EngineRule &p_rule, bool p_in_gear, bool p_clutch_pressed, float p_clutch,
		float p_throttle, float p_wheel_rpm) {
	const float range = p_rule.rpm_max - p_rule.rpm_min;
	float target = 0.0f;
	switch (p_rule.gearbox_mode) {
		case 0:
			target = p_in_gear && !p_clutch_pressed ? p_wheel_rpm
													: std::max(p_rule.rpm_min, p_throttle * p_rule.rpm_max);
			break;
		case 1:
			if (p_in_gear) {
				target = p_clutch_pressed
						? p_rule.rpm_min
						: p_clutch * p_throttle * range + p_rule.rpm_min + (1.0f - p_clutch) * p_wheel_rpm;
			} else {
				target = p_clutch_pressed ? p_rule.rpm_min : p_rule.rpm_min + p_throttle * range;
			}
			break;
		default:
			target = p_in_gear ? p_clutch * p_throttle * range + (1.0f - p_clutch) * p_wheel_rpm + p_rule.rpm_min
							   : p_rule.rpm_min + p_throttle * range;
			break;
	}
	return std::clamp(target, 0.0f, std::max(p_rule.rpm_max, 0.0f));
}

float engine_load(float p_wheel_rpm, float p_rpm, float p_forward_y, float p_forward_speed_sign) {
	const float slope = std::asin(std::clamp(p_forward_y, -1.0f, 1.0f)) * (4.0f / 3.14159265358979323846f) *
			sign_of(p_forward_speed_sign);
	return std::clamp(std::max(slope, 1.0f - p_wheel_rpm / std::max(p_rpm, 1.0f)), 0.0f, 1.0f);
}

float engine_rpm_desired(float p_target, float p_load, float p_floor) {
	return (1.0f - p_load) * (p_target - p_floor) + p_floor;
}

float engine_rpm_step(const EngineRule &p_rule, float p_rpm, float p_desired, float p_floor, float p_dt) {
	const float diff = p_desired - p_rpm;
	const float rate = diff >= 0.0f ? p_rule.internal_acceleration : p_rule.internal_deceleration;
	float delta = std::fabs(diff) * p_dt * rate;
	if (delta < kMinRpmStep) {
		delta = std::min(std::fabs(diff), kMinRpmStep);
	}
	const float rpm = std::max(p_rpm + sign_of(diff) * delta, p_floor);
	return std::min(rpm, p_rule.rpm_max);
}

float wheel_drive_torque(float p_engine_torque, float p_gear_ratio, float p_diff_gear_ratio,
		float p_transmission_efficiency, float p_share) {
	return p_share * kDriveTorqueShare * p_engine_torque * p_gear_ratio * p_diff_gear_ratio *
			p_transmission_efficiency * kDriveTorqueQuarter;
}

float spring_substep_factor(uint32_t p_substeps) {
	static constexpr float kFactors[3] = { 1.0f, 1.5f, 1.75f };
	return kFactors[std::clamp<uint32_t>(p_substeps, 1, 3) - 1];
}

float spring_progressive(float p_compression_ratio, float p_start_ratio, float p_exponent, float p_setting) {
	if (p_compression_ratio <= p_start_ratio || !(p_start_ratio > 0.0f)) {
		return 1.0f;
	}
	return std::pow(p_exponent * p_setting, (p_compression_ratio - p_start_ratio) / p_start_ratio);
}

float brake_scale(float p_angular_velocity, float p_factor, float p_input) {
	if (!(p_input > 0.0f)) {
		return p_angular_velocity;
	}
	return p_angular_velocity * std::clamp(1.0f - p_factor * p_input, 0.0f, 1.0f);
}

float chassis_friction(float p_horizontal_speed_squared, float p_input, float p_normal, float p_low_velocity) {
	return p_horizontal_speed_squared < kStandStillSpeedSquared && p_input >= kStandStillInput ? p_low_velocity
																							   : p_normal;
}

EllipseForces friction_ellipse(float p_longitudinal, float p_lateral, float p_longitudinal_peak, float p_lateral_peak) {
	EllipseForces out{ p_longitudinal, p_lateral };
	if (p_longitudinal_peak != 0.0f) {
		const float x = p_longitudinal / p_longitudinal_peak;
		out.lateral = p_lateral * std::sqrt(std::fabs(1.0f - x * x));
	}
	if (p_lateral_peak != 0.0f) {
		const float y = p_lateral / p_lateral_peak;
		out.longitudinal = p_longitudinal * std::sqrt(std::fabs(1.0f - y * y));
	}
	return out;
}

float clamp_slip(float p_slip) {
	return std::clamp(p_slip, -kSlipClamp, kSlipClamp);
}

float boost_tank_step(const BoostRule &p_rule, float p_tank, bool p_boosting, float p_dt) {
	const float full = std::max(p_rule.dissipation_time, 0.0f);
	const float refill = p_rule.recovery_time > 0.0f ? full / p_rule.recovery_time : full;
	const float tank = p_boosting ? p_tank - p_dt : p_tank + refill * p_dt;
	return std::clamp(tank, 0.0f, full);
}

}
