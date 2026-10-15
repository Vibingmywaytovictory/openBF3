#pragma once
// Damage: falloff, zone multipliers from the level grid, blasts, health (docs/design/m2a-weapons.md §5.9;
// plan §3.2 "Damage", §7.1; topics/01 §2.4, §5.4).
//
//   bullet:  falloff(StartDamage, EndDamage, FalloffStart, FalloffEnd, distance)
//            x grid(projectile property, capsule property).DamageProtectionMultiplier      [which field: inferred]
//            x Mechanics::bullet_damage_modifier                                           (1 on Metro)
//            x the penetration factor accumulated on the way (DamagePenetrationMultiplier per surface passed)
//   direct:  MissileEntityData.Damage (or the 40 mm bullet's StartDamage) x the zone multiplier (2.5 on Metro)
//   blast:   BlastDamage inside InnerBlastRadius, linearly to 0 at BlastRadius (Mechanics::blast_falloff),
//            x the explosion pair's zone multiplier (1.0 on Metro) x SoldierDef ExplosionDamageModifier (1),
//            distance = from the detonation to the nearest point of the target's capsules [inferred], once per target
//   health:  SoldierDef MaxHealth (100); a hit that brings it to <= 0 kills (an exact 100 kills: 80 x 1.25 at 15 m)
// The §7.1 shots-to-kill rows are this arithmetic; shots_to_kill / one_shot_range compute them for a def, and the
// shooting range reproduces them end to end through capsule hits (§10).

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
	uint32_t target = 0; // HitTarget::id
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	uint8_t pellet = 0;
	DamageKind kind = DamageKind::Bullet;
	HitZone zone = HitZone::Body;
	uint8_t capsule = 0;
	uint8_t reserved = 0;
	float distance = 0; // the falloff (or blast) distance used
	float base = 0; // before the zone multiplier
	float multiplier = 1; // the grid's DamageProtectionMultiplier (x the bullet damage modifier)
	float penetration = 1; // DamagePenetrationMultiplier product of what the bullet passed
	float amount = 0; // base x multiplier x penetration
};

// The falloff of a projectile at p_distance (bf3_core gameplay::damage_at, in double).
double falloff_damage(const ProjectileSimDef &p_projectile, double p_distance);
// One bullet hit on a zone before penetration: falloff x the zone cell's multiplier x the damage modifier.
double zone_damage(const ProjectileSimDef &p_projectile, HitZone p_zone, double p_distance, const Mechanics &p_mechanics);
// Blast damage at p_distance (0 beyond the radius).
double blast_damage(const ExplosionSimDef &p_explosion, double p_distance, BlastFalloff p_falloff);

// Applies p_amount to a soldier: health, the damage of the tick, kDamaged / kDied events, MoveMode::Dead.
// Refused (false) while spawn-protected (Mechanics::spawn_protection_blocks) or dead.
bool apply_damage(SoldierState &r_soldier, float p_amount, const Mechanics &p_mechanics);

// Shots to kill a soldier of p_health at p_distance when every pellet of every shot hits p_zone (0 = never:
// a zone multiplier of 0, e.g. a grenade's body). One entry per zone.
std::array<int32_t, kHitZoneCount> shots_to_kill(const WeaponSimDef &p_def, double p_distance, float p_health, const Mechanics &p_mechanics);
// The largest distance at which one shot to p_zone kills (bisection to 1 mm over [0, the def's max range]);
// -1 = at no distance, +infinity = at every distance up to the range.
double one_shot_range(const WeaponSimDef &p_def, HitZone p_zone, float p_health, const Mechanics &p_mechanics);

} // namespace bf3::sim::weapons
