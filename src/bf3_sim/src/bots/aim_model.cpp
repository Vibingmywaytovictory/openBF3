#include "bf3/sim/bots/aim_model.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::bots {

namespace {

constexpr float kMaxConeDeg = 45.0f;

float finite_or(float p_value, float p_fallback) {
	return std::isfinite(p_value) ? p_value : p_fallback;
}

}

float aim_error(const AimParams &p_params, float p_skill, float p_seconds_on_target, bool p_moving, float p_crossing_speed_mps) {
	const float skill = std::clamp(finite_or(p_skill, 0.5f), 0.0f, 1.0f);
	const float t = std::max(finite_or(p_seconds_on_target, 0.0f), 0.0f);
	const float tau = std::max(finite_or(p_params.tau_s, 0.6f), 1e-3f);
	const float e0 = std::max(finite_or(p_params.e0_deg, 0.0f), 0.0f);
	const float e_inf = std::max(finite_or(p_params.e_inf_deg, 0.0f), 0.0f);
	const float k = std::max(0.25f, 1.0f + 2.0f * finite_or(p_params.skill_scale, 1.0f) * (0.5f - skill));
	float sigma = k * (e_inf + (e0 - e_inf) * std::exp(-t / tau));
	if (p_moving) {
		sigma += std::max(finite_or(p_params.moving_penalty_deg, 0.0f), 0.0f);
	}
	sigma += std::max(finite_or(p_params.crossing_deg_per_mps, 0.0f), 0.0f) * std::max(finite_or(std::fabs(p_crossing_speed_mps), 0.0f), 0.0f);
	return std::clamp(finite_or(sigma, kMaxConeDeg), 0.0f, kMaxConeDeg);
}

}
