#pragma once
// Internal to bf3_render (level_scene.cpp, scene_lanes.cpp) and render.scene.unit: the pure steps of the level
// scene, so the unit test runs them on synthetic values without EBX (docs/design/m1.md §14 render.scene.unit).

#include "bf3/core/level/level_transform.h"
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

using Mat3 = std::array<std::array<float, 3>, 3>;

// The role a material's texture parameter gives its texture, and its rank as the material's drawn albedo or normal
// map (0 = the plain Diffuse / Normal, higher = a later fallback, -1 = never drawn; SceneMaterialPlan::albedo).
struct ParameterUse {
	TextureRole role = TextureRole::Data;
	int rank = -1;
	bool drawn() const { return rank >= 0; }
};
ParameterUse parameter_use(std::string_view p_parameter);
// The parameters a slot draws: indices into p_slot.textures of the albedo and the normal map (-1 none).
std::pair<int32_t, int32_t> drawn_parameters(const MaterialSlot &p_slot);
// A texture named by several materials keeps the strongest role: Normal over Color over Data (a normal map must
// not be uploaded as colour, and a colour map must not lose its sRGB / alpha handling to a data role).
TextureRole merge_role(TextureRole p_a, TextureRole p_b);

// What M1 reads from a ShaderGraph partition name (lowercase), docs §11.3: whether the surface draws at all (the
// census's undrawn words: invisible, occluder, shadowmesh, ui_characterbackdrop, ui_menu), alpha scissor for the
// alpha-tested families, double-sided for foliage.
struct ShaderFlags {
	bool drawn = true;
	bool alpha_test = false;
	bool double_sided = false;
};
ShaderFlags shader_flags(std::string_view p_shader);

// One variant from its res's prepared LOD (in place): surfaces whose material slot does not draw (p_surface_materials
// entry kSceneUndrawnMaterial, or an index past the list) are dropped; triangles with a corner in a hidden part are
// removed (the census rule) and the vertices compacted; the bake maps positions by L, normals by L^-T and tangents by
// L (all renormalised), the tangent sign and the winding flip when det L < 0; the AABBs and totals are recomputed.
// Fails on arrays whose sizes disagree (a hostile blob that decode_mesh_blob let through cannot happen, but the
// function never trusts its input).
core::Status apply_scene_variant(PreparedMesh &r_mesh, const std::vector<uint16_t> &p_hidden, const std::optional<Mat3> &p_bake,
		const std::vector<uint32_t> &p_surface_materials);
// Whether a prepared mesh has a triangle.
bool has_triangles(const PreparedMesh &p_mesh);

// Bytes the arrays of a prepared mesh hold (what a SceneSink item keeps in flight).
uint64_t prepared_bytes(const PreparedMesh &p_mesh);
uint64_t prepared_bytes(const PreparedTexture &p_texture);

// The heightfield grid as one Godot surface (positions and normals through p_transform, clockwise front faces seen
// from above, cells with a hole corner left out), restricted to the samples inside [p_min_xz, p_max_xz] (world X/Z
// before the transform; empty = the whole grid). Vertices are only the used samples.
core::Result<PreparedMesh> terrain_mesh(const core::res::HeightGrid &p_grid, const core::level::LevelTransform &p_transform, std::string_view p_name,
		const std::optional<std::array<float, 4>> &p_crop_xz = std::nullopt);

// The depth of the heightfield quadtree whose sample spacing first reaches p_spacing (the soup's rule,
// collision_soup.cpp terrain_grid), capped so the grid side stays within res::kMaxGridSide.
uint8_t terrain_depth(const core::res::HeightfieldTree &p_tree, float p_spacing);

} // namespace bf3::render::scene_internal
