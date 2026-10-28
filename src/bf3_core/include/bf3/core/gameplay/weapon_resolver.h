#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"

#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

struct WeaponResolveOptions {

	std::vector<std::string> unlocks;

	const EbxResolver *ui = nullptr;
};

Result<WeaponDef> resolve_weapon(const EbxResolver &p_db, std::string_view p_partition, const WeaponResolveOptions &p_options = {});

struct WeaponUnlockModifier {
	std::string unlock;
	EbxInstance modifier;
};

Result<std::vector<WeaponUnlockModifier>> weapon_unlock_modifiers(
		const EbxResolver &p_db, std::string_view p_partition, std::string_view p_type);

struct WeaponUnlockSound {
	std::string unlock;
	std::string sound;
	bool operator==(const WeaponUnlockSound &) const = default;
};

Result<std::vector<WeaponUnlockSound>> weapon_unlock_sounds(const EbxResolver &p_db, std::string_view p_partition);

std::vector<std::string> list_weapon_variants(const EbxLibrary &p_library);

std::string base_variant_of(const EbxLibrary &p_library, std::string_view p_blueprint);

std::string weapon_name_sid(const EbxResolver &p_ui, uint32_t p_unlock_identifier);

}
