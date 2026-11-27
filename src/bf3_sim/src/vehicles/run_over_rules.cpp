#include "bf3/sim/vehicles/run_over_rules.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bf3::sim::vehicles {

// INFERRED: the speed is the hull velocity at the contact along the contact normal (its whole speed with
// IgnoreSoldierCollisionNormal); R2-A5 C3 leaves V's meaning open. Settle: a note on that speed.
float run_over_speed(const RunOverRule &p_rule, const Vec3 &p_velocity, const Vec3 &p_normal) {
	if (p_rule.ignore_normal) {
		return std::sqrt(p_velocity[0] * p_velocity[0] + p_velocity[1] * p_velocity[1] + p_velocity[2] * p_velocity[2]);
	}
	return std::max(p_velocity[0] * p_normal[0] + p_velocity[1] * p_normal[1] + p_velocity[2] * p_normal[2], 0.0f);
}

// SOURCE: notes R2-A5 C3 and C4 (1000 above a per-object threshold; x CollisionDamageMultiplier, strictly above
// CollisionDamageThreshold).
// INFERRED: the per-object threshold is KillSoldierCollisionSpeedThreshold, in m/s. Settle: a note naming that field.
// STAND-IN: the impact term below the threshold is not identified and gives 0. Settle: a note decoding that term.
float run_over_damage(
		const RunOverRule &p_rule, float p_speed, float p_collision_multiplier, float p_collision_threshold) {
	const float impact = p_speed > p_rule.kill_speed ? kRunOverKillDamage : 0.0f;
	const float damage = p_collision_multiplier * impact;
	return damage > p_collision_threshold ? damage : 0.0f;
}

// SOURCE: notes R2-A5 C5 (one step's amounts added up).
// INFERRED: a hull counts once at its strongest contact, and the credit goes to the heaviest hull. Settle: a note on both.
RunOverTotal run_over_total(std::span<const ObstacleContact> p_contacts,
		const std::function<std::optional<RunOverHull>(uint32_t)> &p_hull_of) {
	struct Hull {
		uint32_t slot = 0;
		float damage = 0;
	};
	std::vector<Hull> hulls;
	for (const ObstacleContact &c : p_contacts) {
		const std::optional<RunOverHull> hull = p_hull_of(c.slot);
		if (!hull) {
			continue;
		}
		const float damage = run_over_damage(hull->rule, run_over_speed(hull->rule, c.velocity, c.normal),
				hull->collision_multiplier, hull->collision_threshold);
		const auto seen = std::find_if(hulls.begin(), hulls.end(), [&](const Hull &h) { return h.slot == c.slot; });
		if (seen == hulls.end()) {
			hulls.push_back(Hull{ c.slot, damage });
		} else {
			seen->damage = std::max(seen->damage, damage);
		}
	}
	RunOverTotal total;
	float heaviest = 0.0f;
	for (const Hull &h : hulls) {
		total.damage += h.damage;
		if (h.damage > heaviest) {
			heaviest = h.damage;
			total.heaviest = h.slot;
		}
	}
	return total;
}
}
