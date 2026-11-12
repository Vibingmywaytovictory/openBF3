#pragma once

#include "bf3/core/superbundle_mount.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/texture_prep.h"

#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::render {

struct SlotTextureWants {
	std::vector<std::pair<std::string, TextureRole>> wanted;
	std::set<std::string> seen;

	void add(const MeshMaterials &p_materials, std::span<const PreparedSurface> p_surfaces);
};

std::vector<PreparedTexture> load_slot_textures(const core::LevelMount &p_mount, const SlotTextureWants &p_wants,
		std::string_view p_what, std::vector<std::string> &r_notes);

}
