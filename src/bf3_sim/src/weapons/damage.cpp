#include "bf3/sim/weapons/damage.h"

#include "bf3/core/math.h"

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
	if (!p_explosion.present || (p_explosion.radius <= 0.0f && p_explosion.shockwave_radius <= 0.0f)) {
		return 0.0;
	}
	const double d = p_distance > 0 ? p_distance : 0.0;
	if (d > double(p_explosion.gather_radius())) {
		return 0.0;
	}
	const double inner = p_explosion.inner_radius;
	const double outer = p_explosion.radius;
	double blast = 0.0;
	if (d < inner) {
		blast = p_explosion.blast_damage;
	} else if (d <= outer && outer > inner) {
		blast = double(p_explosion.blast_damage) * (1.0 - (d - inner) / (outer - inner));
	}
	return std::max(blast, double(p_explosion.shockwave_damage));
}

double blast_impulse(const ExplosionSimDef &p_explosion, double p_distance) {
	if (!p_explosion.present) {
		return 0.0;
	}
	double s = 0.0;
	if (p_explosion.radius != 0.0f && p_distance <= double(p_explosion.radius)) {
		s = (1.0 - std::max(p_distance, 0.0) / double(p_explosion.radius)) * double(p_explosion.impulse);
	}
	return std::max(s, double(p_explosion.shockwave_impulse));
}

float impulse_size_factor(const ImpulseBox &p_box, const Vec3 &p_direction) {
	const Vec3 d = core::vec::normalize_or(p_direction, Vec3{ 0.0f, 1.0f, 0.0f });
	const Vec3 u = core::vec::normalize_or(core::vec::cross(d, Vec3{ 0.0f, 1.0f, 0.0f }), Vec3{ 1.0f, 0.0f, 0.0f });
	const Vec3 v = core::vec::cross(d, u);
	const auto extent = [&](const Vec3 &p_axis) {
		float e = 0.0f;
		for (size_t i = 0; i < 3; ++i) {
			e += std::fabs(core::vec::dot(p_axis, p_box.axes[i])) * p_box.half[i];
		}
		return 2.0f * e;
	};
	return std::clamp(extent(u) * extent(v), kImpulseAreaMin, kImpulseAreaMax) / kImpulseAreaMax;
}

Vec3 explosion_impulse(
		const ExplosionSimDef &p_explosion, const Vec3 &p_at, const Vec3 &p_reference, const ImpulseBox &p_box) {
	const double s = blast_impulse(p_explosion, double(core::vec::length(core::vec::sub(p_reference, p_at))));
	const Vec3 direction = core::vec::normalize_or(core::vec::sub(p_box.centre, p_at), Vec3{ 0.0f, 1.0f, 0.0f });
	return core::vec::scale(direction, float(s) * impulse_size_factor(p_box, direction));
}

Vec3 crew_throw_impulse(const ExplosionSimDef &p_explosion, const Vec3 &p_draws) {
	const Vec3 direction = core::vec::normalize_or(p_draws, Vec3{ 0.0f, 1.0f, 0.0f });
	return core::vec::scale(direction, kCrewThrowShare * (p_explosion.present ? p_explosion.impulse : 0.0f));
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
