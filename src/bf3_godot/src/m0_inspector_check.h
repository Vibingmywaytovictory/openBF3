#pragma once
// The M0 exit check of the weapon inspector (plan §6: "an in-game inspector prints the resolved
// M16A3/M16A4-burst WeaponDef matching §7.1 and its localised name"). Compares the two WeaponDefs
// the inspector resolved with the §7.1 numbers. No Godot API: the inspector adds its own rows (tr(),
// install snapshot) and prints the table.

#include "bf3/core/gameplay/weapon_def.h"

#include <string>
#include <vector>

namespace bf3::godot_side {

struct M0CheckRow {
	std::string check; // "m16a3.rate_of_fire"
	bool pass = false;
	std::string actual;
	std::string expected;
};

// M16A3 (weapons/m16a4/m16a4 through its base variant) and the M16A4 burst (weapons/m16a4/u_m16_burst)
// against plan §7.1; p_*_name are the localised names the inspector shows.
std::vector<M0CheckRow> check_m0_weapons(const bf3::core::gameplay::WeaponDef &p_m16a3, const std::string &p_m16a3_name,
		const bf3::core::gameplay::WeaponDef &p_m16a4_burst, const std::string &p_m16a4_name);

// A row comparing two strings.
M0CheckRow m0_text_row(const std::string &p_check, const std::string &p_actual, const std::string &p_expected);

} // namespace bf3::godot_side
