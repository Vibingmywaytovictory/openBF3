#pragma once
// The install side of the weapon simulation for the tools and the data tests (bf3dump weapon-sim / range, ctest
// sim.weapons, sim.weapons.range, sim.weapons.fuzz, sim.weapons.perf; docs/design/m2a-weapons.md §10, §14).
//
// One level's weapons through bf3_core's WeaponCatalog (the whole level library parsed once), its material grid
// with the grid pair list (read_grid_pairs), the soldier hit capsules and the SoldierDef, turned into the
// SurfaceMaterialTable the simulation reads, plus WeaponTables of resolved variants. Everything is read through the
// repo's readers from the install as it is (read-only); nothing is cached or written. The game's loader (integration
// wave) builds the same objects from the cached weapon blobs; nothing here is on the replicated path.

#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/weapons/surface_materials.h"
#include "bf3/sim/weapons/weapon_checks.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Install;
}

namespace bf3::sim::weapons {

struct InstallWeapons {
	std::unique_ptr<core::gameplay::WeaponCatalog> catalog;
	std::shared_ptr<const core::gameplay::MaterialGrid> grid;
	std::vector<core::gameplay::MaterialPairRef> grid_pairs; // read_grid_pairs: index = SurfaceTag::material & 0xFFFFFF
	std::vector<core::gameplay::HitCapsule> capsules; // read_hit_capsules (13, 9 hi-LOD)
	core::gameplay::SoldierDef soldier; // read_soldier_def (characters/soldiers/mpsoldier)
	std::shared_ptr<const SurfaceMaterialTable> materials;
	float max_health = 100.0f; // SoldierDef MaxHealth
	float stand_eye_height = 1.6f; // the Stand pose's eye height
	float sprint_recover_seconds = 0.12f; // SoldierSprintSettingsData.RecoverTime when present
	double seconds = 0; // wall time of the open
};

// Opens p_level's weapons (default MP_Subway). Fails when the level, its grid, the grid pairs, the capsules or the
// soldier do not read.
core::Result<InstallWeapons> open_install_weapons(std::shared_ptr<const core::Install> p_install,
		std::string_view p_level = core::gameplay::kDefaultWeaponLevel);

// The options a table of this level is built with (its soldier's sprint recover time) on top of p_mechanics.
WeaponTableOptions install_table_options(const InstallWeapons &p_weapons, const Mechanics &p_mechanics = {}, bool p_infinite_magazine = false);

// The table of p_variants resolved from the catalog (ids in p_variants order); fails on the first variant that does
// not resolve or whose sim def is refused.
core::Result<std::shared_ptr<const WeaponTable>> install_weapon_table(const InstallWeapons &p_weapons, std::span<const std::string_view> p_variants,
		const WeaponTableOptions &p_options);

// The inputs of run_weapon_checks for this level: the gate variants (weapon_check_variants()) resolved, the table
// and its infinite_magazine oracle copy, the soldier's health and stand eye height.
core::Result<WeaponCheckInputs> install_weapon_check_inputs(const InstallWeapons &p_weapons, const Mechanics &p_mechanics = {});

// ---- W4: the generated table of every infantry weapon ----------------------------------------------------------

// The W4 qualifying rule (docs §1): a Bullet projectile, StartDamage > 0, not instant hit, class not wcUgl /
// wcPowerTool (119 of MP_Subway's 172 variants).
bool w4_qualifies(const core::gameplay::WeaponDef &p_def);

inline constexpr std::array<double, 6> kW4Distances{ 0, 10, 25, 50, 75, 100 };

struct W4Row {
	std::string variant;
	std::string weapon_class;
	std::string error; // empty when the row is valid (resolve and sim def built)
	double rpm = 0;
	float muzzle_speed = 0;
	float start_damage = 0, end_damage = 0, falloff_start = 0, falloff_end = 0;
	std::array<float, kHitZoneCount> zone{}; // DamageProtectionMultiplier per zone
	int32_t pellets = 1;
	std::array<std::array<int32_t, kHitZoneCount>, kW4Distances.size()> shots_to_kill{}; // [distance][zone]
	std::array<double, kHitZoneCount> one_shot_range{}; // -1 never, +inf every range
};
struct W4Table {
	size_t variants = 0; // every variant of the level
	size_t qualifying = 0;
	size_t errors = 0; // qualifying variants that did not resolve or whose sim def was refused
	std::vector<W4Row> rows; // qualifying variants, in the catalog's (sorted) order
	double seconds = 0;
};
// Resolves every variant on up to p_threads lanes (0 = all cores; WeaponCatalog::resolve is thread-safe).
W4Table build_w4_table(const InstallWeapons &p_weapons, const Mechanics &p_mechanics = {}, unsigned p_threads = 0);
// The TSV: a header line, then one line per row (stk_<zone>_<distance>, ohk_<zone>).
std::string format_w4_tsv(const W4Table &p_table);

} // namespace bf3::sim::weapons
