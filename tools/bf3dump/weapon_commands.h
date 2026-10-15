#pragma once
// bf3dump weapon commands (plan §3.2, M0 exit "inspector prints the resolved M16A3/M16A4-burst
// WeaponDef matching §7.1"): weapon, weapons. Read through bf3_core's gameplay readers.

#include "ebx_commands.h"

#include "bf3/core/gameplay/weapon_catalog.h"

#include <string>
#include <vector>

namespace bf3dump {

// The level superbundle weapons are read from when --level is not given.
inline constexpr const char *kDefaultWeaponLevel = bf3::core::gameplay::kDefaultWeaponLevel;

// weapon <partition> [--unlock <u_..>]... [--level <sb>] [--name]: one resolved WeaponDef, the level's
// body multipliers for its projectile and the derived numbers; --name adds the localised name.
int cmd_weapon(const EbxContext &p_ctx, const std::vector<std::string> &p_args);
// weapons [--level <sb>]: one TSV row per SoldierWeaponUnlockAsset under weapons/.
int cmd_weapons(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

} // namespace bf3dump
