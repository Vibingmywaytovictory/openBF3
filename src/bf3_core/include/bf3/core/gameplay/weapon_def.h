#pragma once
// WeaponDef: one soldier weapon variant resolved from EBX into plain data (plan §3.2, topics/01 §1,
// §5.1). Nothing here points into a partition; a WeaponDef stays valid after the library is gone.
//
// Source chain (all verified on the retail install, topics/01 §1.2):
//   SoldierWeaponUnlockAsset {Weapon, Extra, Identifier}
//    -> SoldierWeaponBlueprint.Object -> SoldierWeaponData {WeaponFiring, WeaponModifierData[], ...}
//    -> WeaponFiringData {DeployTime, WeaponSway -> GunSwayData, PrimaryFire -> FiringFunctionData}
//    -> FiringFunctionData {Shot, FireLogic, Ammo, OverHeat, WeaponDispersion}
//    -> Shot.ProjectileData -> BulletEntityData | MissileEntityData | GrenadeEntityData (+ explosions,
//       WeaponSuppressionData, MaterialContainerPair)
// Units are the data's: degrees, metres, seconds, m/s, rounds per minute. Angles of the recoil model
// keep the data's sign: horizontal Max = left, -Min = right (topics/01 C7).
//
// What the numbers mean in the game (fire timing, spread decay, recoil recovery) is the simulation's
// business; the few derived helpers at the end are the arithmetic the plan's oracles quote.

#include "bf3/core/ids.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

// FireLogicType, values as stored.
enum class FireLogic : uint8_t {
	Single = 0,
	SingleWithBoltAction = 1,
	Automatic = 2,
	Burst = 3,
	HoldAndRelease = 4,
	Detonated = 5,
	Unknown = 255,
};

// ReloadType, values as stored.
enum class ReloadType : uint8_t {
	SingleBullet = 0,
	Magazine = 1,
	MagazineWithPossibleShorterReload = 2,
	Unknown = 255,
};

enum class Stance : uint8_t { Stand = 0, Crouch = 1, Prone = 2 };
enum class Zoom : uint8_t { NoZoom = 0, Zoom = 1 };
// GunSway move states; crouch and prone carry Base and Moving only (GunSwayBaseMoveData).
enum class SwayMove : uint8_t { Base = 0, Moving, Jumping, Sprinting, VaultingSmall, VaultingMedium, Count };
// GunSwayData stance transitions, in this order.
enum class StanceTransition : uint8_t { ProneToCrouch = 0, ProneToStand, CrouchToProne, CrouchToStand, StandToCrouch, StandToProne, Count };

enum class ProjectileKind : uint8_t { None, Bullet, Missile, Grenade, Other };

using Vec3 = std::array<float, 3>;

// GunSwayDispersionData / FiringDispersionData.
struct Dispersion {
	float min_angle = 0;
	float max_angle = 0;
	float increase_per_shot = 0;
	float decrease_per_second = 0;
};

// GunSwayRecoilData.
struct Recoil {
	float up_per_shot = 0; // RecoilAmplitudeIncPerShot
	float up_max = 0; // RecoilAmplitudeMax
	float horizontal_min = 0; // HorizontalRecoilAmplitudeIncPerShotMin (right = -min)
	float horizontal_max = 0; // HorizontalRecoilAmplitudeIncPerShotMax (left)
	float horizontal_amplitude_max = 0; // HorizontalRecoilAmplitudeMax
	float decrease_factor = 0; // RecoilAmplitudeDecreaseFactor
};

// GunSwayStanceZoomModifierData: multipliers. MaxAngleModifier and Lag*Mod of 0 mean "unset" (every
// attachment modifier and the suppression modifiers carry 0 there; topics/01 extra-7).
struct SwayModifier {
	float min_angle = 1;
	float max_angle = 1;
	float increase_per_shot = 1;
	float decrease_per_second = 1;
	float recoil_magnitude = 1; // vertical recoil per shot
	float recoil_angle = 1; // horizontal recoil per shot
	float lag_yaw = 1;
	float lag_pitch = 1;
};

struct SwayState {
	std::array<Dispersion, size_t(SwayMove::Count)> move{};
	std::array<bool, size_t(SwayMove::Count)> has_move{}; // false: not in the data (crouch/prone jumping...)
	Recoil recoil;
};

// GunSwayModifierData of one attachment or variant, gated by its unlock.
struct SwayGatedModifier {
	std::string unlock; // lowercase partition name of the unlock asset (or "?<guid>" when not mounted)
	Guid unlock_instance;
	bool only_in_supported_shooting = false; // bipod deployed
	bool only_on_weapon_light_enabled = false; // laser / light switched on
	std::array<std::array<SwayModifier, 2>, 3> mod{}; // [stance][zoom]
};

struct GunSway {
	std::string partition;
	std::array<std::array<SwayState, 2>, 3> state{}; // [stance][zoom], after the unconditional modifiers
	struct Transition {
		Dispersion max_penalty;
		float cool_down = 0;
	};
	std::array<Transition, size_t(StanceTransition::Count)> transitions{};
	std::array<SwayModifier, 2> suppression{}; // [zoom]
	float first_shot_recoil_multiplier = 1;
	float shooting_recoil_decrease_scale = 0;
	std::array<float, 2> deviation_scale{ 1, 1 }; // DeviationScaleFactorNoZoom/Zoom
	std::array<float, 2> gameplay_deviation_scale{ 1, 1 };
	// CameraRecoilData (visual spring kick).
	float camera_spring_constant = 0;
	float camera_spring_damping = 0;
	float camera_spring_min_threshold_angle = 0;
	// Modifiers whose unlock is active, already multiplied into `state`.
	std::vector<SwayGatedModifier> applied;
	// Modifiers whose unlock is active but which hold only while supported or with the light on;
	// the simulation applies them when the gate holds.
	std::vector<SwayGatedModifier> conditional;
	size_t modifiers_total = 0; // GunSwayData.Modifiers entries
	size_t modifiers_unresolved = 0; // entries whose partition the resolver does not hold (skipped)
};

// FireLogicData + ShotConfigData + AmmoConfigData + OverHeatData + SoldierWeaponDispersion.
struct FireFunction {
	FireLogic logic = FireLogic::Unknown;
	std::vector<FireLogic> extra_modes; // FireLogicTypeArray: the modes the fire-mode key cycles through
	float rate_of_fire = 0;
	float rate_of_fire_for_burst = 0;
	float client_fire_rate_multiplier = 0;
	float pre_fire_delay = 0;
	float automatic_delay = 0;
	float trigger_pull_weight = 0;
	// BoltActionData (inert defaults on non-bolt weapons).
	float bolt_action_delay = 0;
	float bolt_action_time = 0;
	bool unzoom_on_bolt_action = false;
	bool return_to_zoom_after_bolt_action = false;
	bool hold_bolt_action_until_fire_release = false;
	bool hold_bolt_action_until_zoom_release = false;
	bool force_bolt_action_on_fire_trigger = false;
	// HoldAndReleaseData (grenades).
	float hold_delay = 0;
	float hold_max_time = 0;
	float hold_min_power = 0;
	float hold_max_power = 0;
	float hold_power_per_second = 0;
	float hold_killed_power_modifier = 0;
	bool hold_force_fire_when_killed = false;
	// Reload.
	ReloadType reload_type = ReloadType::Unknown;
	float reload_time = 0; // empty magazine (rtSingleBullet: per shell)
	float reload_time_bullets_left = 0; // tactical
	float reload_delay = 0;
	float reload_threshold = 0;
	bool always_auto_reload = false;
	std::string reload_logic;
	// Ammo.
	int32_t magazine_capacity = 0;
	int32_t number_of_magazines = 0;
	int32_t trace_frequency = 0;
	float auto_replenish_delay = 0;
	// Shot.
	Vec3 initial_position{};
	Vec3 initial_direction{};
	Vec3 initial_speed{}; // muzzle velocity in weapon space (z forward, y up)
	int32_t bullets_per_shell = 0; // pellets
	int32_t bullets_per_shot = 0;
	int32_t bullets_per_burst = 0;
	float spawn_delay = 0;
	float inherit_weapon_speed = 0;
	bool active_force_spawn_to_camera = false;
	bool force_spawn_to_camera = false;
	// Overheat.
	float heat_per_bullet = 0;
	float heat_drop_per_second = 0;
	float overheat_penalty_time = 0;
	float overheat_threshold = 0;
	// SoldierWeaponDispersion: zero on rifles; the pellet cone on shotguns.
	std::array<Dispersion, 3> weapon_dispersion{}; // [stance]
	float jump_dispersion_angle = 0;
	float move_dispersion_angle = 0;
	float move_zoomed_dispersion_angle = 0;
	float weapon_dispersion_decrease_per_second = 0;
	size_t firing_dispersions = 0; // FiringFunctionData.Dispersion entries (empty on soldier weapons)
	// Recoil in FireLogicData (zero on soldier weapons; soldier recoil is in GunSway).
	bool fire_logic_recoil_nonzero = false;
	// M2a weapons (blob v2; docs/design/m2a-weapons.md §4.5): FireLogicData's reload holds (M98B true / true,
	// 870 buckshot false / false [verified, EBX dump 2026-10-15]).
	bool hold_off_reload_until_fire_release = false;
	bool force_reload_action_on_fire_trigger = false;
};

// MaterialContainerPair as referenced by a projectile, explosion or hit capsule.
struct MaterialPairRef {
	bool present = false; // the field holds a reference
	bool resolved = false; // the pair instance was found
	Guid partition;
	Guid instance;
	uint32_t flags_and_index = 0;
	int32_t physics_material_index = 0;
	int32_t physics_property_index = 0; // signed in the data: row = MaterialIndexMap[value & 0xFF]
	uint8_t property() const { return uint8_t(physics_property_index & 0xFF); }
	uint8_t material() const { return uint8_t(physics_material_index & 0xFF); }
	bool penetrable() const { return ((flags_and_index >> 24) & 1u) != 0; }
};

// VeniceExplosionEntityData / ExplosionEntityData.
struct Explosion {
	bool present = false;
	std::string partition;
	float blast_damage = 0;
	float blast_radius = 0;
	float inner_blast_radius = 0;
	float blast_impulse = 0;
	float shockwave_damage = 0;
	float shockwave_radius = 0;
	float shockwave_impulse = 0;
	float shockwave_time = 0;
	float max_occlusion_raycast_radius = 0;
	bool disable_occlusion = false;
	bool disable_static_entity_occlusion = false;
	bool is_causing_suppression = false;
	// M2a weapons (blob v2; docs/design/m2a-weapons.md §4.2, §4.5): the explosion's own MaterialContainerPair
	// (40 mm / RPG 79 BlastDamage, dud 139 Dud, M67 80 Grenade), whose zone cells scale the blast.
	MaterialPairRef material;
};

// WeaponSuppressionData (the per-bullet suppression multiplier, Symthic's "suppression %").
struct Suppression {
	bool present = false;
	float min_multiplier = 0;
	float max_multiplier = 0;
	float min_distance = 0;
	float max_distance = 0;
};

struct ProjectileDef {
	ProjectileKind kind = ProjectileKind::None;
	std::string type; // most derived EBX type
	std::string partition;
	std::string ammunition_type;
	float gravity = 0; // m/s^2 along world Y (negative = down)
	float time_to_live = 0;
	float record_initial_speed = 0; // ProjectileEntityData.InitialSpeed: overridden by the shot speed
	float visual_converge_distance = 0;
	float impact_impulse = 0;
	bool detonate_on_timeout = false;
	bool server_projectile_disabled = false;
	// BulletEntityData.
	float start_damage = 0;
	float end_damage = 0;
	float damage_falloff_start = 0;
	float damage_falloff_end = 0;
	float time_to_arm_explosion = 0;
	float distribute_damage_over_time = 0;
	bool instant_hit = false;
	// MissileEntityData.
	float missile_damage = 0; // direct hit
	float engine_strength = 0;
	float max_speed = 0;
	float engine_time_to_ignition = 0;
	float engine_time_to_live = 0;
	float time_to_arm = 0;
	float drag = 0;
	float max_turn_angle = 0;
	float time_to_activate_guiding = 0;
	// GrenadeEntityData.
	float collision_speed_multiplier = 0;
	float min_bounce_speed = 0;
	std::string grenade_type;
	MaterialPairRef material;
	Explosion explosion;
	Explosion dud_explosion;
	Suppression suppression;
};

struct WeaponDef {
	// Identity.
	std::string variant; // SoldierWeaponUnlockAsset partition the def was resolved for (may be empty)
	uint32_t unlock_identifier = 0; // UnlockAssetBase.Identifier of the variant
	std::string blueprint; // SoldierWeaponBlueprint partition
	std::string blueprint_name; // its Name field ("Weapons/M16A4/M16A4")
	std::string damage_giver_name;
	std::string name_sid; // localisation SID of the weapon name (UIWeaponDescription.Name), "" when not found
	uint32_t name_sid_hash = 0; // loc_sid_hash(name_sid)
	std::vector<std::string> active_unlocks; // partitions, in activation order
	std::vector<std::string> applied_modifiers; // "<unlock>: <ModifierType>" in application order
	// SoldierWeaponData (after modifiers).
	std::string weapon_class;
	std::string anim_base_set;
	std::string weapon_anim_type; // from WeaponAnimTypeModifier, "" when none applies
	std::string aiming_controller; // partition
	std::string sound; // partition of the fire sound
	float render_fov = 0;
	float zoom_render_fov = 0; // 0 = not set by an optic
	bool silenced = false;
	bool enable_breath_control = false;
	bool can_be_in_supported_shooting = false;
	// WeaponFiringData.
	std::string firing_data; // "<partition>#<instance guid>" of the effective WeaponFiringData
	float deploy_time = 0;
	float alt_deploy_time = 0;
	float disable_zoom_on_deploy_time = 0;
	float support_delay_stand = 0;
	float support_delay_prone = 0;
	bool reload_whole_mags = false;
	FireFunction fire;
	ProjectileDef projectile;
	GunSway sway;
};

// --- derived arithmetic (the plan's §7.1 oracles; topics/01 §2.3-§2.5) --------------------------

const char *fire_logic_name(FireLogic p_logic);
const char *reload_type_name(ReloadType p_type);
const char *projectile_kind_name(ProjectileKind p_kind);
FireLogic fire_logic_from(int32_t p_value);

// Seconds between two shots of the primary mode: 60 / RateOfFire, or for single fire with bolt
// action the longer of that and BoltActionDelay + BoltActionTime (the cycle Symthic's effective
// RPM matches; composition [inferred], topics/01 C8).
double shot_interval(const FireFunction &p_fire);
// Seconds between the rounds of a burst: 60 / RateOfFireForBurst.
double burst_interval(const FireFunction &p_fire);
// 60 / shot_interval.
double effective_rpm(const FireFunction &p_fire);
// Muzzle speed: the shot's InitialSpeed.z (the projectile record's own InitialSpeed is overridden).
double muzzle_speed(const WeaponDef &p_weapon);
// Damage at p_distance: StartDamage up to the falloff start, linear to EndDamage at the falloff end,
// flat beyond (linear shape [report claim, Symthic]).
double damage_at(const ProjectileDef &p_projectile, double p_distance);
// Drop below the launch line after p_distance metres with no drag: t = d / v0, 0.5 * |g| * t^2.
double drop_at(double p_speed, double p_gravity, double p_distance);
// Muzzle speed x TimeToLive.
double max_range(const WeaponDef &p_weapon);

// The localisation SID hash of the text database (gap UI §4): h = 0xFFFFFFFF; h = h * 33 + byte.
uint32_t loc_sid_hash(std::string_view p_sid);

// Multiplies one GunSwayModifierData into the stance x zoom states (weapon_resolver.h rule 4):
// dispersion min/+shot/-second always, max unless its modifier is 0; vertical recoil per shot by
// RecoilMagnitudeMod, horizontal min/max by RecoilAngleMod.
void apply_sway_modifier(std::array<std::array<SwayState, 2>, 3> &r_state, const SwayGatedModifier &p_modifier);

} // namespace bf3::core::gameplay
