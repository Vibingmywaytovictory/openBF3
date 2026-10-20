#include "bf3/sim/weapons/damage.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bf3::sim::weapons {

double falloff_damage(const ProjectileSimDef &p_projectile, double p_distance) {

	const double a = p_projectile.falloff_start;
	const double b = p_projectile.falloff_end;
	if (p_distance <= a || b <= a) {
		return p_distance <= a ? p_projectile.start_damage : p_projectile.end_damage;
	}
	if (p_distance >= b) {
		return p_projectile.end_damage;
	}
	const double t = (p_distance - a) / (b - a);
	return double(p_projectile.start_damage) + (double(p_projectile.end_damage) - double(p_projectile.start_damage)) * t;
}

double zone_damage(const ProjectileSimDef &p_projectile, HitZone p_zone, double p_distance, const Mechanics &p_mechanics) {
	const size_t z = std::min<size_t>(size_t(p_zone), kHitZoneCount - 1);

	const double mult = double(p_projectile.zone[z].protection_multiplier) * double(p_mechanics.bullet_damage_modifier);
	return falloff_damage(p_projectile, p_distance) * mult;
}

double blast_damage(const ExplosionSimDef &p_explosion, double p_distance, BlastFalloff p_falloff) {
	(void)p_falloff;
	if (!p_explosion.present) {
		return 0.0;
	}
	const double d = p_distance > 0 ? p_distance : 0.0;
	const double inner = p_explosion.inner_radius;
	const double outer = p_explosion.radius;
	if (outer <= inner) {
		return d <= outer ? double(p_explosion.blast_damage) : 0.0;
	}
	if (d <= inner) {
		return p_explosion.blast_damage;
	}
	if (d >= outer) {
		return 0.0;
	}
	return double(p_explosion.blast_damage) * (outer - d) / (outer - inner);
}

bool apply_damage(SoldierState &r_soldier, float p_amount, const Mechanics &p_mechanics) {
	if (!(p_amount > 0) || r_soldier.mode == MoveMode::Dead || (r_soldier.flags & soldier_flags::kAlive) == 0) {
		return false;
	}
	if (p_mechanics.spawn_protection_blocks && (r_soldier.flags & soldier_flags::kSpawnProtected) != 0) {
		return false;
	}
	r_soldier.health -= p_amount;
	r_soldier.damage += p_amount;
	r_soldier.events = uint16_t(r_soldier.events | soldier_events::kDamaged);
	if (r_soldier.health <= 0.0f) {
		r_soldier.health = 0.0f;
		r_soldier.mode = MoveMode::Dead;
		r_soldier.flags = uint16_t(r_soldier.flags & ~soldier_flags::kAlive);
		r_soldier.events = uint16_t(r_soldier.events | soldier_events::kDied);
	}
	return true;
}

std::array<int32_t, kHitZoneCount> shots_to_kill(const WeaponSimDef &p_def, double p_distance, float p_health, const Mechanics &p_mechanics) {
	std::array<int32_t, kHitZoneCount> out{};
	for (size_t z = 0; z < kHitZoneCount; ++z) {

		const float per = float(zone_damage(p_def.projectile, HitZone(z), p_distance, p_mechanics)) * float(p_def.pellets);
		if (!(per > 0) || !(p_health > 0)) {
			out[z] = 0;
			continue;
		}
		const double estimate = std::ceil(double(p_health) / double(per));
		if (estimate > 1e6) {
			out[z] = 0;
			continue;
		}
		float h = p_health;
		int32_t n = 0;
		while (h > 0.0f && n < 1'000'000) {
			h -= per;
			++n;
		}
		out[z] = n;
	}
	return out;
}

double one_shot_range(const WeaponSimDef &p_def, HitZone p_zone, float p_health, const Mechanics &p_mechanics) {
	const double range = double(p_def.projectile.muzzle_speed) * units_to_seconds(p_def.projectile.time_to_live);
	auto kills = [&](double p_d) { return zone_damage(p_def.projectile, p_zone, p_d, p_mechanics) * double(p_def.pellets) >= double(p_health); };
	if (!kills(0.0)) {
		return -1.0;
	}
	if (kills(range)) {
		return std::numeric_limits<double>::infinity();
	}
	double lo = 0.0, hi = range;
	while (hi - lo > 1e-3) {
		const double mid = 0.5 * (lo + hi);
		(kills(mid) ? lo : hi) = mid;
	}
	return lo;
}

}
