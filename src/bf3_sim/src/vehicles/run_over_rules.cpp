#include "bf3/sim/vehicles/run_over_rules.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::vehicles {

float run_over_speed(const RunOverRule &p_rule, const Vec3 &p_velocity, const Vec3 &p_normal) {
	if (p_rule.ignore_normal) {
		return std::sqrt(p_velocity[0] * p_velocity[0] + p_velocity[1] * p_velocity[1] + p_velocity[2] * p_velocity[2]);
	}
	return std::max(p_velocity[0] * p_normal[0] + p_velocity[1] * p_normal[1] + p_velocity[2] * p_normal[2], 0.0f);
}

float run_over_damage(
		const RunOverRule &p_rule, float p_speed, float p_collision_multiplier, float p_collision_threshold) {
	const float impact = p_speed > p_rule.kill_speed ? kRunOverKillDamage : 0.0f;
	const float damage = p_collision_multiplier * impact;
	return damage > p_collision_threshold ? damage : 0.0f;
}

}
