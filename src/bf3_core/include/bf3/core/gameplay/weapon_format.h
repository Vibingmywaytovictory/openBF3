#pragma once
// The WeaponDef text block (plan §6 M0 exit "an in-game inspector prints the resolved
// M16A3/M16A4-burst WeaponDef matching §7.1 and its localised name"). One formatter shared by
// `bf3dump weapon` and the in-game inspector so both print the same block.
//
// Layout: one "key value" line per group, keys padded to 13 columns, then the gun sway states.
// Numbers are printed with %.7g ("-" for NaN); derived times with fixed decimals. With a text
// database the block carries a "name" line after "name sid": the SID's text, or the SID itself and a
// note when the database has no usable entry (plan §2.1).

#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_def.h"

#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {
class TextDatabase;
}

namespace bf3::core::gameplay {

// %.7g, or "-" for NaN.
std::string format_number(double p_value);
// %.*f.
std::string format_fixed(double p_value, int p_digits);
// "Automatic + [Single]".
std::string format_fire_modes(const FireFunction &p_fire);

// The localised weapon name: the text of p_weapon.name_sid, or the SID itself when the database has
// no usable entry; "" when the weapon has no SID or p_text is null.
std::string weapon_display_name(const WeaponDef &p_weapon, const ui::TextDatabase *p_text);

// The block. p_grid completes the projectile's material pair and, with p_capsules, adds the body
// multipliers; p_text adds the "name" line. Any of them may be null.
std::string format_weapon(const WeaponDef &p_weapon, const MaterialGrid *p_grid, const std::vector<HitCapsule> *p_capsules,
		const ui::TextDatabase *p_text = nullptr);

} // namespace bf3::core::gameplay
