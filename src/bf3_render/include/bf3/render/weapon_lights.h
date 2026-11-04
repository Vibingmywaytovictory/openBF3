#pragma once

#include "bf3/anim/pose_math.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/level/spot_light.h"
#include "bf3/core/result.h"
#include "bf3/render/weapon_sockets.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::render {

struct WeaponSocketLight {
	std::string socket_unlock;
	std::string prefab;
	std::string bone;
	anim::Mat34 transform;
	core::level::SpotLight light;
};

core::Result<std::vector<core::level::SpotLight>> read_prefab_spot_lights(
		const core::EbxResolver &p_db, std::string_view p_prefab);

core::Result<std::vector<WeaponSocketLight>> weapon_socket_lights(const core::EbxResolver &p_db,
		std::span<const WeaponSocket> p_sockets, const WeaponAccessories &p_accessories, bool p_first_person);

float spot_light_half_angle_deg(const core::level::SpotLight &p_light);

}
