#pragma once

#include "bf3/anim/vault_anim.h"
#include "bf3/render/viewmodel_set.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::render {

struct ViewmodelVaultClips {
	std::array<ViewmodelClip, anim::kVaultAnimKinds> clips;
	std::array<anim::VaultPlay, anim::kVaultAnimKinds> play{};
	std::array<std::string, anim::kVaultAnimKinds> path;
};

void load_viewmodel_vault(const core::ant::AntLibrary &p_library, const anim::Skeleton &p_rig,
		const anim::VaultControllers &p_controllers, uint32_t p_ant_ref, ViewmodelItem &r_item,
		std::vector<std::string> &r_notes);

anim::VmVault viewmodel_vault(const anim::Skeleton &p_rig, const ViewmodelItem &p_item);

}
