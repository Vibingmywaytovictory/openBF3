#pragma once

#include "bf3/core/gameplay/weapon_def.h"

#include <string>
#include <vector>

namespace bf3::godot_side {

struct M0CheckRow {
	std::string check;
	bool pass = false;
	std::string actual;
	std::string expected;
};

std::vector<M0CheckRow> check_m0_weapons(const bf3::core::gameplay::WeaponDef &p_m16a3, const std::string &p_m16a3_name,
		const bf3::core::gameplay::WeaponDef &p_m16a4_burst, const std::string &p_m16a4_name);

M0CheckRow m0_text_row(const std::string &p_check, const std::string &p_actual, const std::string &p_expected);

}
