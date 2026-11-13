#pragma once

#include "bf3/sim/sim_types.h"

namespace bf3::sim::vehicles {

inline constexpr float kRunOverKillDamage = 1000.0f;

struct RunOverRule {
	float kill_speed = 0;
	bool ignore_normal = false;
};

float run_over_speed(const RunOverRule &p_rule, const Vec3 &p_velocity, const Vec3 &p_normal);

float run_over_damage(
		const RunOverRule &p_rule, float p_speed, float p_collision_multiplier, float p_collision_threshold);

}
