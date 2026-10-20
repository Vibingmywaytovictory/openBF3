#pragma once

#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_def.h"

#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {
class TextDatabase;
}

namespace bf3::core::gameplay {

std::string format_number(double p_value);

std::string format_fixed(double p_value, int p_digits);

std::string format_fire_modes(const FireFunction &p_fire);

std::string weapon_display_name(const WeaponDef &p_weapon, const ui::TextDatabase *p_text);

std::string format_weapon(const WeaponDef &p_weapon, const MaterialGrid *p_grid, const std::vector<HitCapsule> *p_capsules,
		const ui::TextDatabase *p_text = nullptr);

}
