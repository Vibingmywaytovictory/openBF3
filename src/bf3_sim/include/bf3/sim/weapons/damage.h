#pragma once

#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/mechanics.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>

namespace bf3::sim::weapons {

enum class DamageKind : uint8_t { Bullet = 0, Direct = 1, Blast = 2, DudBlast = 3 };

struct DamageEvent {
	SimTime time = 0;
	uint32_t attacker_slot = 0;
	uint32_t target = 0;
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	uint8_t pellet = 0;
	DamageKind kind = DamageKind::Bullet;
	HitZone zone = HitZone::Body;
	uint8_t capsule = 0;
	uint8_t reserved = 0;
	float distance = 0;
	float base = 0;
	float multiplier = 1;
	float penetration = 1;
	float amount = 0;
};

double falloff_damage(const ProjectileSimDef &p_projectile, double p_distance);

double zone_damage(const ProjectileSimDef &p_projectile, HitZone p_zone, double p_distance, const Mechanics &p_mechanics);

double blast_damage(const ExplosionSimDef &p_explosion, double p_distance, BlastFalloff p_falloff);

inline constexpr float kImpulseAreaMin = 4.0f;
inline constexpr float kImpulseAreaMax = 10.0f;
inline constexpr float kCrewThrowShare = 0.3f;

struct ImpulseBox {
	Vec3 centre{};
	std::array<Vec3, 3> axes{ Vec3{ 1, 0, 0 }, Vec3{ 0, 1, 0 }, Vec3{ 0, 0, 1 } };
	Vec3 half{ 0.5f, 0.5f, 0.5f };
};

double blast_impulse(const ExplosionSimDef &p_explosion, double p_distance);
float impulse_size_factor(const ImpulseBox &p_box, const Vec3 &p_direction);
Vec3 explosion_impulse(
		const ExplosionSimDef &p_explosion, const Vec3 &p_at, const Vec3 &p_reference, const ImpulseBox &p_box);
Vec3 crew_throw_impulse(const ExplosionSimDef &p_explosion, const Vec3 &p_draws);

bool apply_damage(SoldierState &r_soldier, float p_amount, const Mechanics &p_mechanics);

std::array<int32_t, kHitZoneCount> shots_to_kill(const WeaponSimDef &p_def, double p_distance, float p_health, const Mechanics &p_mechanics);

double one_shot_range(const WeaponSimDef &p_def, HitZone p_zone, float p_health, const Mechanics &p_mechanics);

}
