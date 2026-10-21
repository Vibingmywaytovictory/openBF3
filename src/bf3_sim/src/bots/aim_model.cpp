#include "bf3/sim/bots/aim_model.h"

#include "bf3/core/util/finite.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::bots {

namespace {

constexpr float kMaxConeDeg = 45.0f;
constexpr float kMaxLeadSeconds = 5.0f;
constexpr int kLeadIterations = 3;

float finite_or(float p_value, float p_fallback) {
	return std::isfinite(p_value) ? p_value : p_fallback;
}

using bf3::core::finite;
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

Vec3 lead_point(const Vec3 &p_origin, const Vec3 &p_target, const Vec3 &p_target_velocity, float p_muzzle_speed, float p_gravity) {
	if (!finite(p_origin) || !finite(p_target) || !(p_muzzle_speed > 0.0f) || !std::isfinite(p_muzzle_speed)) {
		return p_target;
	}
	const Vec3 v = finite(p_target_velocity) ? p_target_velocity : Vec3{};
	const float g = finite_or(p_gravity, 0.0f);
	Vec3 aim = p_target;
	float t = 0.0f;
	for (int i = 0; i < kLeadIterations; ++i) {
		const float dx = aim[0] - p_origin[0], dy = aim[1] - p_origin[1], dz = aim[2] - p_origin[2];
		t = std::min(std::sqrt(dx * dx + dy * dy + dz * dz) / p_muzzle_speed, kMaxLeadSeconds);
		aim = { p_target[0] + v[0] * t, p_target[1] + v[1] * t, p_target[2] + v[2] * t };
	}
	aim[1] += -0.5f * g * t * t;
	return finite(aim) ? aim : p_target;
}

}
