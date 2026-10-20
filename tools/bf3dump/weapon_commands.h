#pragma once

#include "ebx_commands.h"

#include "bf3/core/gameplay/weapon_catalog.h"

#include <string>
#include <vector>

namespace bf3dump {

inline constexpr const char *kDefaultWeaponLevel = bf3::core::gameplay::kDefaultWeaponLevel;

int cmd_weapon(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

int cmd_weapons(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

}
