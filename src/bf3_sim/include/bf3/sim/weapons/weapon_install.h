#pragma once

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
	std::vector<core::gameplay::MaterialPairRef> grid_pairs;
	std::vector<core::gameplay::HitCapsule> capsules;
	core::gameplay::SoldierDef soldier;
	std::shared_ptr<const SurfaceMaterialTable> materials;
	float max_health = 100.0f;
	float stand_eye_height = 1.6f;
	float sprint_recover_seconds = 0.12f;
	double seconds = 0;
};

core::Result<InstallWeapons> open_install_weapons(std::shared_ptr<const core::Install> p_install,
		std::string_view p_level = core::gameplay::kDefaultWeaponLevel);

WeaponTableOptions install_table_options(const InstallWeapons &p_weapons, const Mechanics &p_mechanics = {}, bool p_infinite_magazine = false);

core::Result<std::shared_ptr<const WeaponTable>> install_weapon_table(const InstallWeapons &p_weapons, std::span<const std::string_view> p_variants,
		const WeaponTableOptions &p_options);

core::Result<WeaponCheckInputs> install_weapon_check_inputs(const InstallWeapons &p_weapons, const Mechanics &p_mechanics = {});

bool w4_qualifies(const core::gameplay::WeaponDef &p_def);

inline constexpr std::array<double, 6> kW4Distances{ 0, 10, 25, 50, 75, 100 };

struct W4Row {
	std::string variant;
	std::string weapon_class;
	std::string error;
	double rpm = 0;
	float muzzle_speed = 0;
	float start_damage = 0, end_damage = 0, falloff_start = 0, falloff_end = 0;
	std::array<float, kHitZoneCount> zone{};
	int32_t pellets = 1;
	std::array<std::array<int32_t, kHitZoneCount>, kW4Distances.size()> shots_to_kill{};
	std::array<double, kHitZoneCount> one_shot_range{};
};
struct W4Table {
	size_t variants = 0;
	size_t qualifying = 0;
	size_t errors = 0;
	std::vector<W4Row> rows;
	double seconds = 0;
};

W4Table build_w4_table(const InstallWeapons &p_weapons, const Mechanics &p_mechanics = {}, unsigned p_threads = 0);

std::string format_w4_tsv(const W4Table &p_table);

}
