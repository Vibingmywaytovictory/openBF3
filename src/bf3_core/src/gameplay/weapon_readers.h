#pragma once

#include "ebx_fields.h"

#include "bf3/core/gameplay/weapon_def.h"

#include <string>
#include <vector>

namespace bf3::core::gameplay::detail {

struct ActiveUnlocks {
	std::vector<Guid> guids;
	std::vector<std::string> names;

	bool contains(const Guid &p_guid) const;

	bool add(const Guid &p_guid, std::string p_name);
};

void read_gun_sway(Fields &r_f, const EbxInstance &p_sway, const ActiveUnlocks &p_active, GunSway &r_sway);

void read_fire_function(Fields &r_f, const EbxInstance &p_ffd, FireFunction &r_fire);

void read_projectile(Fields &r_f, const EbxInstance &p_projectile, ProjectileDef &r_projectile);

}
