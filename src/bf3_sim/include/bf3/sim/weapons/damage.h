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

bool apply_damage(SoldierState &r_soldier, float p_amount, const Mechanics &p_mechanics);

std::array<int32_t, kHitZoneCount> shots_to_kill(const WeaponSimDef &p_def, double p_distance, float p_health, const Mechanics &p_mechanics);

double one_shot_range(const WeaponSimDef &p_def, HitZone p_zone, float p_health, const Mechanics &p_mechanics);

}
