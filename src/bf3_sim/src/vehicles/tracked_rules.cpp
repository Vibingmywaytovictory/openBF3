#include "bf3/sim/vehicles/tracked_rules.h"

#include "bf3/sim/vehicles/land_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::vehicles {

bool track_steer_wheel(int p_steering_type, int p_driving_type) {
	return p_steering_type == 2 && p_driving_type == 1;
}

float track_steer_target(float p_steer, float p_side_sign, float p_wheel_speed, float p_engine_factor) {
	const float fix =
			1.0f - std::clamp(kTankSteerFixMultiplier * std::fabs(p_wheel_speed) - kTankSteerFixOffset, 0.0f, 1.0f);
	return p_steer * p_side_sign * kTankSteer * kTrackSteerForce * fix * p_engine_factor;
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

float track_speed_attenuation(float p_limit, float p_vehicle_speed) {
	return 1.0f - std::clamp(p_limit * std::fabs(p_vehicle_speed) / 2.0f, 0.0f, 1.0f);
}

}
