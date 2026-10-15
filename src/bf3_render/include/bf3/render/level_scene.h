#pragma once
// The level scene for the game (plan §2.5 "world drawing", §6 M1 "Godot RS instances"; docs/design/m1.md §11).
// Godot-free; the plan is made on the loader thread that owns the level database, the meshes and textures are
// prepared on the loader lanes, and the Godot side (src/bf3_godot level_session) only creates RenderingServer
// resources and instances from the result, in <= 2 ms slices on the main thread.
//
// Plan (from the census, bf3_core level/census.h): one SceneMeshPlan per kept census variant (res + hidden parts
// + bake + variation entry), one SceneInstancePlan per census instance, one SceneMaterialPlan per distinct
// (variant, material slot), one SceneTexturePlan per distinct texture res the materials name, with the role the
// first material gave it (Normal wins over Color: a normal map must not be uploaded as colour).
//
// Meshes go through the same MeshRequest as `bf3dump prepare` (scene_mesh_requests is the one place that decides
// LODs and filters; level_prepare.cpp calls it too), so a game load after `bf3dump prepare` hits every entry.
// M1 uploads LOD 0 only (docs §11.2: the M1 budget is load time and RSS, not draw distance); the blob still holds
// the census LODs so M2 adds visibility ranges without a re-convert.
//
// Textures are decoded from the install at every load (plan §2.4: never cached), resident, full chain.

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/result.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_cache.h"
#include "bf3/render/texture_prep.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core {
class Cache;
class EbxResolver;
class LevelMount;
} // namespace bf3::core
namespace bf3::core::level {
class LoadedLevel;
class World;
struct MeshCensus;
} // namespace bf3::core::level

namespace bf3::render {

// The request of every distinct MeshSet res of the kept census variants, in first-placement order: LODs = the
// census's drawing LODs (<= 4) when any variant of the res is model kind, LOD 0 when all are world kind; drawn
// subsets; unskinned. Shared with prepare_level (so their keys match).
struct SceneMeshRequest {
	std::string res; // MeshSet res name
	MeshRequest request;
	std::vector<uint32_t> variants; // indices into MeshCensus::meshes using this res
};
std::vector<SceneMeshRequest> scene_mesh_requests(const core::level::MeshCensus &p_census);

struct ScenePlanOptions {
	bool include_hidden_start = false; // placements that start invisible (starts_hidden) stay out (default)
	uint32_t max_lod = 0; // M1: LOD 0 only
};

struct SceneTexturePlan {
	std::string name; // DxTexture res name
	TextureRole role = TextureRole::Color;
};

struct SceneMaterialPlan {
	MaterialSlot slot; // shader, texture parameters, vector parameters
	std::vector<int32_t> textures; // indices into LevelScenePlan::textures, in slot.textures order (-1 none)
	bool alpha_test = false; // the shader family's alpha (foliage, fences); M1 decides by shader name, docs §11.3
	bool double_sided = false;
};

struct SceneMeshPlan {
	uint32_t request = 0; // index into LevelScenePlan::requests (the res blob)
	uint32_t census_mesh = 0; // index into MeshCensus::meshes
	std::vector<uint16_t> hidden_parts; // composite parts removed at preparation (sorted)
	std::optional<std::array<std::array<float, 3>, 3>> bake; // linear map baked into the vertices
	std::vector<uint32_t> surface_materials; // per MeshAsset.Materials index -> LevelScenePlan::materials
	bool world_kind = false;
	uint32_t instances = 0;
};

struct SceneInstancePlan {
	uint32_t mesh = 0; // index into LevelScenePlan::meshes
	core::level::LevelTransform transform; // unit rotation x uniform scale + translation (bake is in the mesh)
};

// The sun and the ambient of the mode's VisualEnvironment (OutdoorLight, Sky, Fog components). Fields the data
// does not give keep these defaults (docs §11.4 lists what is read and what is invented).
struct SceneEnvironment {
	bool from_data = false;
	std::array<float, 3> sun_direction{ -0.35f, -0.8f, 0.45f }; // pointing from the sun, normalised
	std::array<float, 3> sun_color{ 1.0f, 0.96f, 0.9f };
	float sun_energy = 1.0f;
	std::array<float, 3> sky_color{ 0.45f, 0.55f, 0.7f };
	std::array<float, 3> ground_color{ 0.25f, 0.24f, 0.22f };
	float ambient_energy = 0.6f;
	float fog_start = 0.0f; // 0 = no fog
	float fog_end = 0.0f;
	std::array<float, 3> fog_color{ 0.6f, 0.65f, 0.7f };
};

struct LevelScenePlan {
	std::vector<SceneMeshRequest> requests;
	std::vector<SceneMeshPlan> meshes;
	std::vector<SceneInstancePlan> instances;
	std::vector<SceneMaterialPlan> materials;
	std::vector<SceneTexturePlan> textures;
	SceneEnvironment environment;
	std::vector<std::pair<std::string, std::string>> failures; // (item, error): materials not found, ...
	double seconds = 0.0;
};

// On the loader thread that owns the level database (World is not thread-safe). Materials come from each
// variant's MeshVariationDatabaseEntry (MeshCensus::SceneMesh::entry) through p_resolver, else the MeshAsset's own
// MeshMaterials, as material_lookup.h does for an EbxLibrary.
core::Result<LevelScenePlan> plan_level_scene(const core::EbxResolver &p_resolver, const core::level::World &p_world,
		const core::level::MeshCensus &p_census, const ScenePlanOptions &p_options = {});
core::Result<LevelScenePlan> plan_level_scene(const core::level::LoadedLevel &p_level, const core::level::World &p_world,
		const core::level::MeshCensus &p_census, const ScenePlanOptions &p_options = {});

// One scene mesh ready for upload: LOD 0 of its res blob (through the cache), hidden parts' triangles removed, the
// bake applied to positions / normals / tangents (winding flipped when its determinant is negative).
struct PreparedSceneMesh {
	uint32_t mesh = 0; // index into LevelScenePlan::meshes
	PreparedMesh lod0;
	core::CachedItem item; // the blob's cache outcome (hit / converted / uncached)
};
// Any loader lane; p_mount and p_cache are thread-safe for reads. Meshes that share a request share its blob:
// the caller groups by SceneMeshPlan::request so a blob is decoded once per lane (docs §12.2).
core::Result<PreparedSceneMesh> prepare_scene_mesh(core::Cache *p_cache, const core::LevelMount &p_mount, const LevelScenePlan &p_plan,
		uint32_t p_mesh, const PreparedMeshSet &p_blob);
// The blob of one request (cache hit or convert + put).
core::Result<CachedMesh> load_scene_request(core::Cache *p_cache, const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_request);
// Any loader lane: the texture from the install (DxTexture res + chunk), prepared for its role.
core::Result<PreparedTexture> prepare_scene_texture(const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_texture);

// Bytes the plan's prepared data needs resident at once (meshes LOD 0 + textures, estimated from the census):
// the loader uses it to decide the number of lanes against the RSS budget (docs §12.4).
uint64_t estimate_scene_bytes(const LevelScenePlan &p_plan, const core::level::MeshCensus &p_census);

} // namespace bf3::render
