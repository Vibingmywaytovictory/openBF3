#pragma once

#include "bf3/sim/weapons/shooting_range.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::sim::weapons {

struct WeaponCheckRow {
	std::string gate;
	std::string name;
	bool pass = false;
	std::string measured;
	std::string threshold;
	bool unmeasured = false;
	bool reported = false;
};

struct WeaponCheckInputs {
	std::shared_ptr<const WeaponTable> table;
	std::shared_ptr<const WeaponTable> oracle;
	float health = 100.0f;
	float eye_height = 1.6f;
	uint64_t match_seed = 1;
};

std::span<const std::string_view> weapon_check_variants();

std::vector<WeaponCheckRow> run_weapon_checks(const WeaponCheckInputs &p_inputs);

std::string format_weapon_checks(std::span<const WeaponCheckRow> p_rows);

}
