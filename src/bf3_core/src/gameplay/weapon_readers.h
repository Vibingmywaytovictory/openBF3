#pragma once
// Readers for the parts of a WeaponDef (gun sway, firing function, projectile), shared by the
// resolver. Each reads through a detail::Fields and leaves its first problem there.

#include "ebx_fields.h"

#include "bf3/core/gameplay/weapon_def.h"

#include <string>
#include <vector>

namespace bf3::core::gameplay::detail {

// The unlocks a resolution activated: instance GUIDs and their partition names, in order.
struct ActiveUnlocks {
	std::vector<Guid> guids;
	std::vector<std::string> names;

	bool contains(const Guid &p_guid) const;
	// False when already present.
	bool add(const Guid &p_guid, std::string p_name);
};

// GunSwayData (+ CameraRecoilData) and its modifiers: those of active unlocks are applied or, when
// gated, kept in r_sway.conditional.
void read_gun_sway(Fields &r_f, const EbxInstance &p_sway, const ActiveUnlocks &p_active, GunSway &r_sway);
// FiringFunctionData except Shot.ProjectileData (read_projectile).
void read_fire_function(Fields &r_f, const EbxInstance &p_ffd, FireFunction &r_fire);
// A ProjectileEntityData (bullet, missile, grenade or other) with explosions and suppression.
void read_projectile(Fields &r_f, const EbxInstance &p_projectile, ProjectileDef &r_projectile);

} // namespace bf3::core::gameplay::detail
