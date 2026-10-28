#pragma once

#include "bf3/anim/pose_math.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::render {

inline constexpr std::string_view kDefaultSocketUnlockPrefix = core::gameplay::kNoAccessoryUnlockPrefix;

struct WeaponSocketObject {
	std::string mesh_1p, mesh_1p_zoom, mesh_3p;
	bool skinned = false;
	anim::Mat34 transform;
	std::optional<anim::Mat34> transform_3p;
};

struct WeaponSocket {
	std::string unlock;
	std::string bone;
	bool hide_by_zoom_transition = false;
	bool hide_by_light_toggle = false;
	std::vector<WeaponSocketObject> objects;
	std::vector<std::string> skipped;
};

struct WeaponAccessories {
	std::vector<std::string> active;
	std::vector<std::string> listed;

	bool operator==(const WeaponAccessories &) const = default;
};

std::vector<WeaponSocket> read_weapon_sockets(const core::EbxResolver &p_db, const core::EbxInstance &p_swd);

bool socket_drawn(std::string_view p_unlock, const WeaponAccessories &p_accessories);

std::vector<std::string> weapon_modifier_unlocks(const WeaponAccessories &p_accessories);

}
