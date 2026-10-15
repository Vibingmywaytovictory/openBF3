#pragma once
// MeshSet LOD -> vertex/index arrays in the layout Godot's ArrayMesh takes (plan §2.5 "world drawing", "skinned
// soldiers"). Godot-free; runs on loader threads, and the bridge copies the arrays into Packed*Arrays on the main
// thread.
//
// Conventions (README "Resource decoders"; probe §7):
// * Frostbite and Godot are both Y up, metres: positions, normals and tangents are copied 1:1.
// * Winding: the decoder's front faces are counter-clockwise; Godot culls counter-clockwise faces as back faces
//   (its front faces are clockwise), so every triangle is emitted as (i0, i2, i1).
// * Tangents: Godot's ARRAY_TANGENT is xyz + binormal sign and Godot computes binormal = cross(normal, tangent)
//   * sign, the same formula as Frostbite's BinormalSign, so the sign is copied (missing sign = +1).
// * UVs: both use Direct3D's convention (v down), copied as stored.
// * Skinning: 4 influences per vertex (ARRAY_BONES / ARRAY_WEIGHTS). A vertex lane value indexes the subset's
//   bone_slots; lane_binds[subset][lane] maps it to a bind index of the shared skin (bf3::anim::SkinBinds).
//   bones_per_vertex 1 without stored weights gets weight 1 on lane 0. Unused lanes are bone 0 with weight 0.

#include "bf3/core/res/mesh_set.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf3::render {

struct PreparedSurface {
	size_t subset = 0;
	uint32_t material_index = 0; // into MeshAsset.Materials
	std::string material_name; // the MeshSet subset's material name (MeshMaterial binding, for logs and caches)
	std::optional<core::res::SubsetCategory> category;
	std::vector<float> positions; // xyz per vertex
	std::vector<float> normals; // xyz (empty when the subset has none)
	std::vector<float> tangents; // xyzw, w = binormal sign (empty without normals or tangents)
	std::vector<float> uv0; // uv (empty when absent)
	std::vector<float> uv1;
	std::vector<uint8_t> colors; // rgba
	std::vector<int32_t> bones; // 4 per vertex (skinned only)
	std::vector<float> weights; // 4 per vertex (skinned only)
	std::vector<uint16_t> parts; // composite meshes: the part of each vertex (hidden-part filtering), else empty
	std::vector<int32_t> indices; // clockwise triangles
	std::array<float, 3> aabb_min{};
	std::array<float, 3> aabb_max{};

	size_t vertex_count() const { return positions.size() / 3; }
};

struct PreparedMesh {
	std::string name;
	bool skinned = false;
	std::vector<PreparedSurface> surfaces;
	size_t vertices = 0;
	size_t triangles = 0;
};

// Decodes LOD p_lod and converts it. p_lane_binds (skinned meshes) as from SkinBinds::bind_lod; nullptr leaves
// the surfaces unskinned.
core::Result<PreparedMesh> prepare_mesh(const core::res::LoadedMeshSet &p_mesh, size_t p_lod, core::res::SubsetFilter p_filter,
		const std::vector<std::vector<int32_t>> *p_lane_binds);

} // namespace bf3::render
