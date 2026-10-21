#pragma once

#include "bf3/sim/weapons/weapon_install.h"
#include "bf3/sim_harness/weapon_checks.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::sim::weapons {

core::Result<WeaponCheckInputs> install_weapon_check_inputs(
		const InstallWeapons &p_weapons, const Mechanics &p_mechanics = {});

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
