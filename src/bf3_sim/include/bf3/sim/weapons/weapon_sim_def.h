#pragma once

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/mechanics.h"
#include "bf3/sim/weapons/sim_time.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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

inline constexpr uint8_t kMaxShotsPerTick = 8;
inline constexpr int32_t kMaxBulletsPerBurst = 32;
inline constexpr int32_t kMaxPellets = 64;
inline constexpr int32_t kMaxMagazine = 10'000;
inline constexpr int32_t kMaxMagazines = 1'000;
inline constexpr float kMaxSpeed = 5'000.0f;
inline constexpr float kMaxGravity = 100.0f;
inline constexpr float kMaxDamage = 100'000.0f;
inline constexpr float kMaxRadius = 100.0f;
inline constexpr float kMaxAngle = 90.0f;
inline constexpr float kMaxRate = 10'000.0f;
inline constexpr SimTime kMaxFlightTime = 30 * kTimeUnitsPerSecond;
inline constexpr int32_t kMaxTraceFrequency = 1'000;
inline constexpr float kMaxPower = 10.0f;

struct ExplosionSimDef {
	bool present = false;
	float blast_damage = 0;
	float inner_radius = 0;
	float radius = 0;
	float impulse = 0;
	float shockwave_damage = 0;
	float shockwave_radius = 0;
	float shockwave_impulse = 0;
	SimTime spawn_delay = 0;
	float max_occlusion_ray_radius = 0;
	bool disable_occlusion = false;
	bool disable_static_entity_occlusion = false;

	std::array<float, kHitZoneCount> zone_multiplier{ 1, 1, 1, 1 };
	std::array<float, kHitZoneCount> zone_threshold{};
	bool zone_multiplier_from_data = false;

	float gather_radius() const { return std::max({ inner_radius, radius, shockwave_radius }); }
};

struct ProjectileSimDef {
	ProjectileKind kind = ProjectileKind::None;

	Vec3 launch_velocity{};
	Vec3 launch_offset{};
	float muzzle_speed = 0;
	float gravity = 0;
	SimTime time_to_live = 0;
	bool instant_hit = false;

	float start_damage = 0;
	float end_damage = 0;
	float falloff_start = 0;
	float falloff_end = 0;

	SimTime time_to_arm = 0;
	bool detonate_on_timeout = false;
	float direct_damage = 0;

	float engine_acceleration = 0;
	float max_speed = 0;
	SimTime engine_ignition = 0;
	SimTime engine_time_to_live = 0;
	float drag = 0;
	bool gravity_when_guided = false;

	float bounce_speed_multiplier = 0;
	float min_bounce_speed = 0;

	uint8_t property = 0;
	uint8_t physics_material = 0;
	bool never_penetrate_any = false;
	std::array<core::gameplay::DamageRelation, kHitZoneCount> zone{};
	ExplosionSimDef explosion;
	ExplosionSimDef dud;

	float suppression_min = 0;
	float suppression_max = 0;
	float suppression_min_distance = 0;
	float suppression_max_distance = 0;
};

struct FireModeDef {
	FireLogic logic = FireLogic::Unknown;
	SimTime interval = 0;
	SimTime burst_interval = 0;
	float interval_seconds = 0;
	float burst_interval_seconds = 0;
	int32_t bullets_per_burst = 1;
};

struct WeaponSimDef {
	std::string variant;
	uint32_t unlock_identifier = 0;
	std::string weapon_class;

	std::array<FireModeDef, 4> modes{};
	uint8_t mode_count = 0;

	SimTime bolt_cycle = 0;
	bool bolt_hold_until_fire_release = false;
	bool bolt_hold_until_zoom_release = false;
	bool unzoom_on_bolt = false;
	SimTime pre_fire_delay = 0;
	SimTime spawn_delay = 0;

	SimTime hold_delay = 0;
	SimTime hold_max_time = 0;
	float hold_min_power = 1;
	float hold_max_power = 1;
	float hold_power_per_second = 0;

	int32_t magazine_capacity = 0;
	int32_t magazines = 0;
	bool unlimited_ammo = false;
	bool infinite_reserve = false;
	int32_t pellets = 1;

	int32_t trace_frequency = 0;

	ReloadType reload_type = ReloadType::Unknown;
	SimTime reload_tactical = 0;
	SimTime reload_empty = 0;
	SimTime reload_delay = 0;
	float reload_threshold = 1;

	SimTime reload_commit_tactical = 0;
	SimTime reload_commit_empty = 0;
	bool switch_cancels_reload = true;
	bool always_auto_reload = false;

	bool hold_off_reload_until_fire_release = false;

	SimTime deploy_time = 0;
	SimTime zoom_blocked_after_deploy = 0;

	std::array<std::array<SwayState, 2>, 3> sway{};
	std::array<SwayModifier, 2> suppression_modifier{};
	struct Conditional {
		bool only_supported = false;
		bool only_light = false;
		std::array<std::array<SwayModifier, 2>, 3> mod{};
	};
	std::vector<Conditional> conditional;
	float first_shot_recoil_multiplier = 1;
	float shooting_recoil_decrease_scale = 0;

	std::array<float, 2> deviation_scale{ 1, 1 };
	std::array<float, 2> gameplay_deviation_scale{ 1, 1 };
	float camera_spring_constant = 0;
	float camera_spring_damping = 0;
	float camera_spring_min_threshold_angle = 0;

	std::array<Dispersion, 3> pellet_cone{};

	ProjectileSimDef projectile;
};

inline constexpr size_t kMaxWeaponVariants = 255;
inline constexpr size_t kMaxWeaponVariantCombos = 4096;
inline constexpr size_t kNoVariantOption = SIZE_MAX;

inline bool fires_round(const ProjectileSimDef &p_projectile) {
	return p_projectile.kind != ProjectileKind::None && p_projectile.kind != ProjectileKind::Other;
}

struct WeaponTable {
	std::vector<WeaponSimDef> defs;
	std::vector<std::vector<WeaponSimDef>> variants;
	std::shared_ptr<const SurfaceMaterialTable> materials;
	Mechanics mechanics;

	bool infinite_magazine = false;

	bool exact_aim = false;

	SimTime sprint_recover_time = 7'200'000;
	uint64_t hash = 0;

	const WeaponSimDef &def(uint16_t p_def, uint8_t p_variant) const {
		if (p_variant != 0 && p_def < variants.size() && p_variant <= variants[p_def].size()) {
			return variants[p_def][p_variant - 1u];
		}
		return defs[p_def];
	}
};
inline constexpr uint16_t kNoWeapon = 0xFFFF;

core::Result<WeaponSimDef> make_weapon_sim_def(const core::gameplay::WeaponDef &p_def, const SurfaceMaterialTable &p_materials);

core::Result<ExplosionSimDef> make_explosion_sim_def(
		const core::gameplay::Explosion &p_explosion, const SurfaceMaterialTable &p_materials);

struct WeaponTableOptions {
	Mechanics mechanics;
	bool infinite_magazine = false;
	bool exact_aim = false;
	float sprint_recover_seconds = 0.12f;
	std::span<const std::vector<WeaponSimDef>> variants;
};

struct WeaponVariantPart {
	std::vector<std::string> unlocks;
	size_t base = kNoVariantOption;
};

struct WeaponVariantMap {
	std::vector<std::vector<uint8_t>> classes;
	std::vector<uint8_t> radix;
	std::vector<uint8_t> combo_variant;

	bool operator==(const WeaponVariantMap &) const = default;
};

struct WeaponVariants {
	WeaponVariantMap map;
	std::vector<WeaponSimDef> defs;
	size_t resolves = 0;
};

using WeaponVariantResolve =
		std::function<core::Result<core::gameplay::WeaponDef>(const std::vector<std::string> &p_unlocks)>;

core::Result<WeaponVariants> make_weapon_variants(const WeaponSimDef &p_plain,
		std::span<const WeaponVariantPart> p_parts, const WeaponVariantResolve &p_resolve,
		const SurfaceMaterialTable &p_materials);

uint8_t weapon_variant(const WeaponVariantMap &p_map, std::span<const size_t> p_options);

uint64_t weapon_sim_def_hash(const WeaponSimDef &p_def);

std::vector<std::string> weapon_sim_def_diff(const WeaponSimDef &p_a, const WeaponSimDef &p_b);

core::Result<std::shared_ptr<const WeaponTable>> make_weapon_table(std::span<const core::gameplay::WeaponDef> p_defs,
		std::shared_ptr<const SurfaceMaterialTable> p_materials, const WeaponTableOptions &p_options = {});

uint64_t weapon_table_hash(const WeaponTable &p_table);

double sim_shot_interval_seconds(const WeaponSimDef &p_def, uint8_t p_mode = 0);
double sim_effective_rpm(const WeaponSimDef &p_def, uint8_t p_mode = 0);

}
