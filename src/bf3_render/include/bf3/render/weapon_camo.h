#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/render/weapon_sockets.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::render {

inline constexpr std::string_view kCamoTextureParameter = "Camo";
inline constexpr std::string_view kCamoTilingParameter = "CamoTiling";

struct WeaponCamo {
	std::string unlock;
	std::string variation;
	uint32_t variation_hash = 0;
};

std::optional<WeaponCamo> weapon_camo(
		const core::EbxResolver &p_db, std::string_view p_unlock, std::string_view p_weapon_blueprint);

bool unlock_links_nothing(const core::EbxResolver &p_db, std::string_view p_unlock);

uint32_t weapon_camo_hash(
		const core::EbxResolver &p_db, const WeaponAccessories &p_accessories, std::string_view p_weapon_blueprint);

}
