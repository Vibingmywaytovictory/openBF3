#include "bf3/sim/vehicles/tracked_rules.h"

#include "bf3/sim/vehicles/land_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::vehicles {

bool track_steer_wheel(int p_steering_type, int p_driving_type) {
	return p_steering_type == 2 && p_driving_type == 1;
}

float track_forward_term(float p_engine_torque, float p_gear_ratio) {
	return p_engine_torque * p_gear_ratio * kTrackForwardQuarter * kTrackForwardShare * kTankForward;
}

float track_turn_term(float p_yaw, float p_gear_ratio, float p_throttle, bool p_negative_x_side, bool p_inverted) {
	const float direction = p_gear_ratio < 0.0f && p_throttle != 0.0f ? -1.0f : 1.0f;
	const float side = p_negative_x_side ? 1.0f : -1.0f;
	const float inverted = p_inverted ? -1.0f : 1.0f;
	return p_yaw * direction * side * inverted * kTankSteer * kTrackSteerForce;
}

float track_slip_fade(float p_slip) {
	return 1.0f - std::clamp(kTankSteerFixMultiplier * std::fabs(p_slip) - kTankSteerFixOffset, 0.0f, 1.0f);
}

float track_steer_inertia(const TrackSteerRule &p_rule, float p_speed) {
	const std::span<const std::array<float, 2>> c = p_rule.steer_inertia;
	if (c.empty()) {
		return 1.0f;
	}
	const float kmh = std::fabs(p_speed) * kKmhPerMs;
	float v = c.back()[1];
	if (kmh <= c.front()[0]) {
		v = c.front()[1];
	} else {
		for (size_t i = 1; i < c.size(); ++i) {
			if (kmh <= c[i][0]) {
				const float span = c[i][0] - c[i - 1][0];
				v = span > 0.0f ? c[i - 1][1] + (c[i][1] - c[i - 1][1]) * (kmh - c[i - 1][0]) / span : c[i][1];
				break;
			}
		}
	}
	return std::max(v, 1.0f);
}

float track_steer_smooth(float p_previous, float p_target, float p_inertia) {
	float previous = p_previous;
	if (previous * p_target < 0.0f) {
		previous = 0.0f;
	}
	if (std::fabs(p_target) <= std::fabs(previous)) {
		return p_target;
	}
	const float change = p_target - previous;
	if (std::fabs(change) < kTrackSteerWholeChange) {
		return p_target;
	}
	return previous + change / std::max(p_inertia, 1.0f);
}

float track_speed_attenuation(float p_limit, float p_angular_speed) {
	return 1.0f - std::clamp(p_limit * kTrackSpinShare * std::fabs(p_angular_speed), 0.0f, 1.0f);
}

float boost_drive_factor(float p_acceleration_scale, float p_strength) {
	return 1.0f + p_acceleration_scale * (p_strength - 1.0f);
}
}
