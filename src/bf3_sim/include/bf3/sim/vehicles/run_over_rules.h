#pragma once

#include "bf3/sim/sim_types.h"
#include "bf3/sim/vehicles/vehicle_obstacles.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace bf3::sim::vehicles {

inline constexpr float kRunOverKillDamage = 1000.0f;

struct RunOverRule {
	float kill_speed = 0;
	bool ignore_normal = false;
};

float run_over_speed(const RunOverRule &p_rule, const Vec3 &p_velocity, const Vec3 &p_normal);

float run_over_damage(
		const RunOverRule &p_rule, float p_speed, float p_collision_multiplier, float p_collision_threshold);

struct RunOverHull {
	RunOverRule rule;
	float collision_multiplier = 0;
	float collision_threshold = 0;
};

struct RunOverTotal {
	float damage = 0;
	std::optional<uint32_t> heaviest;
};

RunOverTotal run_over_total(std::span<const ObstacleContact> p_contacts,
		const std::function<std::optional<RunOverHull>(uint32_t)> &p_hull_of);
}
