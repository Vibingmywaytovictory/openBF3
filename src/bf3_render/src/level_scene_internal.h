#pragma once

#include "bf3/core/level/level_transform.h"
#include "bf3/core/math.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/result.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/texture_prep.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::render::scene_internal {

using Mat3 = std::array<core::Vec3, 3>;

struct ParameterUse {
	TextureRole role = TextureRole::Data;
	int rank = -1;
	bool drawn() const { return rank >= 0; }
};
ParameterUse parameter_use(std::string_view p_parameter);

std::pair<int32_t, int32_t> drawn_parameters(const MaterialSlot &p_slot);

TextureRole merge_role(TextureRole p_a, TextureRole p_b);

struct ShaderFlags {
	bool drawn = true;
	bool alpha_test = false;
	bool double_sided = false;
};
ShaderFlags shader_flags(std::string_view p_shader);

core::Status apply_scene_variant(PreparedMesh &r_mesh, const std::vector<uint16_t> &p_hidden, const std::optional<Mat3> &p_bake,
		const std::vector<uint32_t> &p_surface_materials);

bool has_triangles(const PreparedMesh &p_mesh);

uint64_t prepared_bytes(const PreparedMesh &p_mesh);
uint64_t prepared_bytes(const PreparedTexture &p_texture);

core::Result<PreparedMesh> terrain_mesh(const core::res::HeightGrid &p_grid, const core::level::LevelTransform &p_transform, std::string_view p_name,
		const std::optional<std::array<float, 4>> &p_crop_xz = std::nullopt);

uint8_t terrain_depth(const core::res::HeightfieldTree &p_tree, float p_spacing);

}
