#pragma once
// WeaponSimDef: the numbers the weapon simulation runs on, derived once per level load from a resolved
// bf3_core WeaponDef (weapon_def.h, the cached "weapon" blob) plus the level's SurfaceMaterialTable
// (docs/design/m2a-weapons.md §5.3, §6).
//
// Why a second struct: WeaponDef is the data as read (floats in seconds, strings, the whole sway tree, every
// modifier); the simulation needs validated, bounded values in sub-tick units, the zone relations of its
// projectile already looked up, and one hash both ends compare (MatchInfo's sim digest, plan §2.7). Building it
// is cheap (microseconds per weapon) and pure, so it is not cached: the WeaponDef it comes from is (§8).
//
// Every field is checked by make_weapon_sim_def (hostile-input bounds, docs §9): finite, non-negative where the
// data is, within the k* limits below; anything else fails with an error naming the variant and the field, so the
// simulation never sees NaN, an endless burst, a zero interval or a 10^9-pellet shot.

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/mechanics.h"
#include "bf3/sim/weapons/sim_time.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::sim::weapons {

class SurfaceMaterialTable;

using core::gameplay::Dispersion;
using core::gameplay::FireLogic;
using core::gameplay::ProjectileKind;
using core::gameplay::Recoil;
using core::gameplay::ReloadType;
using core::gameplay::SwayModifier;
using core::gameplay::SwayMove;
using core::gameplay::SwayState;

// Bounds of a def (retail maxima in brackets, measured over the 172 MP_Subway variants on 2026-10-15).
inline constexpr uint8_t kMaxShotsPerTick = 8; // so the interval must be >= tick / 8 [smallest: 1000 RPM = 3.6 ticks]
inline constexpr int32_t kMaxBulletsPerBurst = 32; // [3]
inline constexpr int32_t kMaxPellets = 64; // [12]
inline constexpr int32_t kMaxMagazine = 10'000; // [100]
inline constexpr int32_t kMaxMagazines = 1'000; // [7]
inline constexpr float kMaxSpeed = 5'000.0f; // m/s, muzzle and missile max speed [650 bullets, 100 missiles]
inline constexpr float kMaxGravity = 100.0f; // |m/s^2| [15]
inline constexpr float kMaxDamage = 100'000.0f; // [500: C4 / M15 blasts]
inline constexpr float kMaxRadius = 100.0f; // m [10: shockwave]
inline constexpr float kMaxAngle = 90.0f; // degrees, every spread / recoil / cone value [10: recoil caps]
inline constexpr float kMaxRate = 10'000.0f; // degrees per second, decreases and factors [30]
inline constexpr SimTime kMaxFlightTime = 30 * kTimeUnitsPerSecond; // a projectile with TimeToLive 0 (40 mm) [10 s missiles]

// Explosion (VeniceExplosionEntityData) with the zone relation of its own material pair looked up.
struct ExplosionSimDef {
	bool present = false;
	float blast_damage = 0;
	float inner_radius = 0;
	float radius = 0;
	float impulse = 0;
	float max_occlusion_ray_radius = 0;
	bool disable_occlusion = false;
	bool disable_static_entity_occlusion = false;
	// DamageProtectionMultiplier of the explosion's pair against each zone; 1.0 until the WeaponDef carries the
	// explosion's MaterialPair (docs §6.4: Metro's blast cells are 1.0 for every zone [verified, grid dump]).
	std::array<float, kHitZoneCount> zone_multiplier{ 1, 1, 1, 1 };
	bool zone_multiplier_from_data = false;
};

struct ProjectileSimDef {
	ProjectileKind kind = ProjectileKind::None;
	// Launch, in the aim frame (x left, y up, z forward; ShotConfigData, Frostbite weapon space): the velocity's
	// length is the muzzle speed (InitialSpeed.z for bullets: the record's own 350 is overridden, topics/01 C3).
	Vec3 launch_velocity{};
	Vec3 launch_offset{}; // ShotConfigData.InitialPosition (M320 (-0.1,-0.3,0.86), RPG (-0.3,-0.1,2))
	float muzzle_speed = 0;
	float gravity = 0; // m/s^2 along world Y (negative = down); no drag on bullets (no field exists)
	SimTime time_to_live = 0; // TimeToLive; 0 in data (40 mm) -> kMaxFlightTime
	bool instant_hit = false; // knives: one segment of length speed x TTL at the shot time
	// Bullet damage (BulletEntityData): StartDamage to the falloff start, linear to EndDamage at the end, flat beyond.
	float start_damage = 0;
	float end_damage = 0;
	float falloff_start = 0;
	float falloff_end = 0;
	// Explosive projectiles.
	SimTime time_to_arm = 0; // BulletEntityData.TimeToArmExplosion / MissileEntityData.TimeToArm; before it: the dud
	bool detonate_on_timeout = false; // grenades: the fuse is TimeToLive (M67 3.6 s)
	float direct_damage = 0; // MissileEntityData.Damage (225 on RPG / SMAW, the community says 100: H:W7)
	// Missile engine (MissileEntityData): thrust along the velocity [inferred] from ignition for its time to live,
	// up to max_speed; drag (0 on RPG / SMAW) as a = -drag x v [inferred, inactive on every M2a kit weapon].
	float engine_acceleration = 0; // EngineStrength, read as m/s^2 [inferred]
	float max_speed = 0;
	SimTime engine_ignition = 0;
	SimTime engine_time_to_live = 0;
	float drag = 0;
	// Grenade bounce (GrenadeEntityData) [inferred: v' = reflect(v) x CollisionSpeedMultiplier, rests below
	// MinBounceSpeed].
	float bounce_speed_multiplier = 0;
	float min_bounce_speed = 0;
	// Its own material pair (grid row of the damage lookups) and the zone relations against the soldier capsules.
	uint8_t property = 0;
	uint8_t physics_material = 0;
	bool never_penetrate_any = false; // every cell of its row has NeverPenetrate (buckshot, melee, explosives)
	std::array<core::gameplay::DamageRelation, kHitZoneCount> zone{};
	ExplosionSimDef explosion;
	ExplosionSimDef dud;
	// WeaponSuppressionData, carried on every shot for the later suppression model (M7): 7 on 5.56, 35 bolt/.338.
	float suppression_min = 0;
	float suppression_max = 0;
	float suppression_min_distance = 0;
	float suppression_max_distance = 0;
};

// One fire mode of the weapon (FireLogicType + the numbers that time it).
struct FireModeDef {
	FireLogic logic = FireLogic::Unknown;
	SimTime interval = 0; // 60 / RateOfFire (Automatic, Single, Burst between bursts)
	SimTime burst_interval = 0; // 60 / RateOfFireForBurst
	int32_t bullets_per_burst = 1;
};

struct WeaponSimDef {
	std::string variant; // the SoldierWeaponUnlockAsset partition (bf3dump and logs)
	uint32_t unlock_identifier = 0;
	std::string weapon_class; // wc556x45mmNATO, wcUgl, wcHgr ...

	// Fire modes: [0] = FireLogicType, then FireLogicTypeArray in data order (the fire-mode counter cycles them).
	std::array<FireModeDef, 4> modes{};
	uint8_t mode_count = 0;
	// Bolt / pump (SingleFireWithBoltAction): the cycle is BoltActionDelay + BoltActionTime (topics/01 C8).
	SimTime bolt_cycle = 0;
	bool bolt_hold_until_fire_release = false; // M98B true, 870 false [verified, EBX dump 2026-10-15]
	bool bolt_hold_until_zoom_release = false;
	bool unzoom_on_bolt = false;
	SimTime pre_fire_delay = 0;
	SimTime spawn_delay = 0; // ShotConfigData.SpawnDelay: the projectile leaves this long after the shot (M67 0.17, knife 0.3)
	// Hold and release (grenades): the throw needs the trigger held for hold_delay; release throws.
	SimTime hold_delay = 0;
	SimTime hold_max_time = 0;
	float hold_min_power = 1;
	float hold_max_power = 1;
	float hold_power_per_second = 0;

	// Ammunition.
	int32_t magazine_capacity = 0; // 31 = 30 + chambered (Symthic; whether an empty reload yields 30 is a rule, §5.5)
	int32_t magazines = 0; // spare magazines (NumberOfMagazines)
	bool unlimited_ammo = false; // NumberOfMagazines < 0 in data (knife: 1 x -1)
	int32_t pellets = 1; // NumberOfBulletsPerShell
	int32_t trace_frequency = 0; // every n-th shot carries a tracer (VFX)

	// Reload (ReloadType, ReloadTime*, ReloadThreshold, ReloadLogic).
	ReloadType reload_type = ReloadType::Unknown;
	SimTime reload_tactical = 0; // ReloadTimeBulletsLeft (rtMagazineWithPossibleShorterReload with rounds left)
	SimTime reload_empty = 0; // ReloadTime (rtSingleBullet: per shell)
	SimTime reload_delay = 0; // ReloadDelay (rtSingleBullet: before the first shell, 870 1.15 s)
	float reload_threshold = 1; // ReloadThreshold: the fraction after which the reload is committed [inferred, H:W9]
	bool switch_cancels_reload = true; // rlWeaponSwitchCancelsUnfinishedReload
	bool always_auto_reload = false;

	// Deploy (WeaponFiringData).
	SimTime deploy_time = 0;
	SimTime zoom_blocked_after_deploy = 0; // DisableZoomOnDeployTime

	// Spread and recoil (GunSwayData after the unconditional modifiers; [stance][zoom]).
	std::array<std::array<SwayState, 2>, 3> sway{};
	std::array<SwayModifier, 2> suppression_modifier{}; // [zoom], MaxAngleModifier 0 = unset (topics/01 extra-7)
	struct Conditional {
		bool only_supported = false; // bipod
		bool only_light = false; // laser / light on
		std::array<std::array<SwayModifier, 2>, 3> mod{};
	};
	std::vector<Conditional> conditional; // GunSway::conditional, applied while the gate holds
	float first_shot_recoil_multiplier = 1;
	float shooting_recoil_decrease_scale = 0; // 0.1 on every rifle dumped: recovery while firing [inferred]
	// RecoveryShape::Exponential's per-tick factors, exp(-RecoilAmplitudeDecreaseFactor / kSimHz), idle and
	// while firing (x ShootingRecoilDecreaseScale), [stance][zoom]: std::exp runs here, never per tick (plan R4).
	std::array<std::array<float, 2>, 3> recovery_factor_idle{};
	std::array<std::array<float, 2>, 3> recovery_factor_firing{};
	// Pellet cone per stance (SoldierWeaponDispersion; 870 buckshot 3 deg); zero on everything but shotguns.
	std::array<Dispersion, 3> pellet_cone{};

	ProjectileSimDef projectile;
};

// The defs one match runs, the materials they were built against and the choices of the unmeasured mechanics
// (mechanics.h). Immutable once built; shared by the server thread and the client prediction thread.
struct WeaponTable {
	std::vector<WeaponSimDef> defs; // index = the uint16 def id in every state and event
	std::shared_ptr<const SurfaceMaterialTable> materials;
	Mechanics mechanics;
	// RecoveryShape::Exponential's per-tick factor exp(-factor x scale / kSimHz) is precomputed per def and recoil
	// state (in the def build, not on the replicated path: plan R4, as make_motor_params does for the gains).
	uint64_t hash = 0; // weapon_table_hash
};
inline constexpr uint16_t kNoWeapon = 0xFFFF;

// Derives and checks one def. p_materials resolves the projectile's (and explosions') pairs and zone relations.
core::Result<WeaponSimDef> make_weapon_sim_def(const core::gameplay::WeaponDef &p_def, const SurfaceMaterialTable &p_materials);
// The table for a loadout universe (the level's kits, or one weapon for the tools).
core::Result<std::shared_ptr<const WeaponTable>> make_weapon_table(std::span<const core::gameplay::WeaponDef> p_defs,
		std::shared_ptr<const SurfaceMaterialTable> p_materials, const Mechanics &p_mechanics = {});
// FNV-1a 64 over every def field's bytes in declaration order (strings as length + bytes), the materials'
// zone properties and the variant choices: the server's and a client's tables must hash equal (MatchInfo).
uint64_t weapon_table_hash(const WeaponTable &p_table);

// Arithmetic of a def the oracles quote (plan §7.1); same rules as the simulation.
double sim_shot_interval_seconds(const WeaponSimDef &p_def, uint8_t p_mode = 0); // incl. the bolt cycle
double sim_effective_rpm(const WeaponSimDef &p_def, uint8_t p_mode = 0);

} // namespace bf3::sim::weapons
