#pragma once
// The level scene for the game (plan §2.5 "world drawing", §6 M1 "Godot RS instances"; docs/design/m1.md §11).
// Godot-free; the plan is made on the loader thread that owns the level database, the meshes, textures and
// terrain are prepared on the loader lanes, and the Godot side (src/bf3_godot level_upload) creates the
// RenderingServer resources from the result: textures (texture_2d_create) and meshes (mesh_create_from_surfaces)
// on WorkerThreadPool tasks, which gap load §6.6 allows; materials and instances on the main thread with a
// per-frame budget (12 ms behind the loading screen, vsync off; 2 ms for later in-game streaming; docs §12.1).
//
// Plan (from the census, bf3_core level/census.h): one SceneMeshPlan per kept census variant (res + hidden parts
// + bake + variation entry) with a shown placement, one SceneInstancePlan per census instance that does not start
// hidden, one SceneMaterialPlan per distinct material slot content (shader, textures, vectors; variants that share
// a look share the material), one SceneTexturePlan per distinct texture res a material draws (its albedo or normal
// map, SceneMaterialPlan::albedo / normal), with the strongest role any material gave it (Normal wins over Color: a
// normal map must not be uploaded as colour).
//
// Meshes go through the same MeshRequest as `bf3dump prepare` (scene_mesh_requests is the one place that decides
// LODs and filters; level_prepare.cpp calls it too), so a game load after `bf3dump prepare` hits every entry.
// M1 uploads LOD 0 only (docs §11.2: the M1 budget is load time and RSS, not draw distance); the blob still holds
// the census LODs so M2 adds visibility ranges without a re-convert.
//
// Textures are decoded from the install at every load (plan §2.4: never cached), resident, full chain.
//
// Terrain: each TerrainEntityData of the census (Metro TDM: one) becomes one untextured world mesh from its
// heightfield (res/terrain.h height_grid at about SceneTerrainOptions::spacing, hole cells dropped), built every
// load on a lane and drawn with one flat material: M1's "minimal look" has outdoor ground. Water is not drawn
// in M1 (no water surface is on a TDM walking route; the shot harness views avoid it, docs §11.2).

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/result.h"
#include "bf3/render/look_environment.h"
#include "bf3/render/look_materials.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_cache.h"
#include "bf3/render/texture_prep.h"

#include <array>
#include <cstdint>
#include <memory>
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

// L1 look (README "L1 look"): which look the plan describes. M1 = the minimal look of docs/design/m1.md §11.3/§11.4,
// kept for the A/B frame-time gate and the before/after shots (`--look=m1`); L1 = materials through the level shader
// database (look_materials.h) and the environment read per component with its areas (look_environment.h).
enum class SceneLook : uint8_t {
	M1,
	L1,
};

struct ScenePlanOptions {
	bool include_hidden_start = false; // placements that start invisible (starts_hidden) stay out (default)
	uint32_t max_lod = 0; // M1: LOD 0 only
	bool include_terrain = true;
	SceneLook look = SceneLook::L1;
	// L1: the mount the shader database and texture headers are read from (the LoadedLevel overload sets it; without
	// one the L1 materials fall back to the M1 rule and the plan lists a failure).
	const core::LevelMount *mount = nullptr;
};

// One TerrainEntityData to draw (docs §11.2).
struct SceneTerrainPlan {
	uint32_t object = 0; // index into World::objects
	std::string asset; // Terrain res name (TerrainAsset)
	core::level::LevelTransform transform;
	std::array<float, 3> color{ 0.36f, 0.33f, 0.27f }; // flat albedo [inferred: no terrain shading in M1]
};
struct SceneTerrainOptions {
	float spacing = 4.0f; // target metres between drawn samples (the collision soup uses 2 m)
};

struct SceneTexturePlan {
	std::string name; // DxTexture res name
	TextureRole role = TextureRole::Color;
};

struct SceneMaterialPlan {
	MaterialSlot slot; // shader, texture parameters, vector parameters
	std::vector<int32_t> textures; // indices into LevelScenePlan::textures, in slot.textures order (-1: not uploaded)
	// The two textures the M1 material draws (indices into LevelScenePlan::textures, -1 none), picked by parameter
	// name: albedo Diffuse, else MainDiffuse, TileDiffuse, DiffuseLeaves, DiffuseBark, Diffuse2; normal map Normal,
	// else MainNormal, TileNormal. Only these are uploaded (docs §11.3). Metro's facade and tile shaders name no plain
	// Diffuse, so a Diffuse-only lookup (render_bridge make_standard_material) would leave them untextured.
	int32_t albedo = -1;
	int32_t normal = -1;
	bool alpha_test = false; // the shader family's alpha (foliage, fences); M1 decides by shader name, docs §11.3
	bool double_sided = false;
	// L1 look: how the two textures are sampled (look_materials.h kLookUv*: UV set; a multiplier), alpha blending
	// (the shader database's transparent surface types) and glass (drawn translucent at a fixed opacity).
	uint8_t albedo_uv = kLookUv0;
	float albedo_scale = 1.0f;
	uint8_t normal_uv = kLookUv0;
	float normal_scale = 1.0f;
	bool blend = false;
	bool glass = false;
	std::optional<LookMaterial> look; // the resolver's answer (L1 only), for the census
};

// SceneMeshPlan::surface_materials entry of a slot whose shader does not draw (the census's undrawn shader words):
// prepare_scene_mesh drops its surfaces, as it drops a surface whose material index is past the list.
inline constexpr uint32_t kSceneUndrawnMaterial = 0xFFFFFFFFu;

struct SceneMeshPlan {
	uint32_t request = 0; // index into LevelScenePlan::requests (the res blob)
	uint32_t census_mesh = 0; // index into MeshCensus::meshes
	std::vector<uint16_t> hidden_parts; // composite parts removed at preparation (sorted)
	std::optional<std::array<std::array<float, 3>, 3>> bake; // linear map baked into the vertices
	std::vector<uint32_t> surface_materials; // per MeshAsset.Materials index -> LevelScenePlan::materials (or kSceneUndrawnMaterial)
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
	std::vector<SceneTerrainPlan> terrains;
	SceneEnvironment environment;
	std::vector<std::pair<std::string, std::string>> failures; // (item, error): materials not found, ...
	double seconds = 0.0;
	SceneLook look = SceneLook::L1;
	LookStats look_stats; // L1: shader database and resolver figures
	// L1: the visual environment with its areas, horizon colours and LUTs decoded, and the base sky's textures as
	// indices into `textures` (-1 none).
	LookEnvironment look_environment;
	int32_t sky_panorama = -1, sky_panorama_alpha = -1, sky_gradient = -1;
};

// On the loader thread that owns the level database (World is not thread-safe). Materials come from each
// variant's MeshVariationDatabaseEntry (MeshCensus::SceneMesh::entry) through p_resolver, else the MeshAsset's own
// MeshMaterials, as material_lookup.h does for an EbxLibrary.
core::Result<LevelScenePlan> plan_level_scene(const core::EbxResolver &p_resolver, const core::level::World &p_world,
		const core::level::MeshCensus &p_census, const ScenePlanOptions &p_options = {});
core::Result<LevelScenePlan> plan_level_scene(const core::level::LoadedLevel &p_level, const core::level::World &p_world,
		const core::level::MeshCensus &p_census, const ScenePlanOptions &p_options = {});

// One scene mesh ready for upload: LOD 0 of its res blob (through the cache), hidden parts' triangles removed, the
// bake applied to positions / normals / tangents (winding flipped when its determinant is negative). LOD 0 is the
// blob's first LOD that still draws a triangle after that (the census keeps a model mesh whose LOD 0 draws nothing
// when LOD 1 draws, census.h).
struct PreparedSceneMesh {
	uint32_t mesh = 0; // index into LevelScenePlan::meshes
	PreparedMesh lod0;
	core::CachedItem item; // the blob's cache outcome (hit / converted / uncached); SceneLanes sets it from load_scene_request
};
// Any loader lane; p_mount and p_cache are thread-safe for reads. Meshes that share a request share its blob:
// the caller groups by SceneMeshPlan::request so a blob is decoded once per lane (docs §12.2).
core::Result<PreparedSceneMesh> prepare_scene_mesh(core::Cache *p_cache, const core::LevelMount &p_mount, const LevelScenePlan &p_plan,
		uint32_t p_mesh, const PreparedMeshSet &p_blob);
// The blob of one request (cache hit or convert + put).
core::Result<CachedMesh> load_scene_request(core::Cache *p_cache, const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_request);
// Any loader lane: the texture from the install (DxTexture res + chunk), prepared for its role.
core::Result<PreparedTexture> prepare_scene_texture(const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_texture);

// Any loader lane: terrain p_terrain's heightfield as one surface (positions, normals; no UVs or tangents),
// wound for Godot (render_bridge.h), hole cells left out. Decoded at every load (Metro: a few MiB of chunks).
core::Result<PreparedMesh> prepare_scene_terrain(const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_terrain,
		const SceneTerrainOptions &p_options = {});

// Bytes the plan's prepared data would hold if all of it were resident at once (each plan mesh's drawn LOD at the
// PreparedMesh layout + each texture's full Godot chain), from the res headers alone (no chunk is read; about one
// small res read per request and texture): the loader uses it to decide the number of lanes against the RSS budget
// (docs §12.4). Items whose res does not read count 0. (Design revision 4 had the census as the input; the census
// holds no vertex counts or texture sizes, docs §11.2 T5 note.)
uint64_t estimate_scene_bytes(const LevelScenePlan &p_plan, const core::LevelMount &p_mount);

// ---- Phase B: the scene lanes (docs §12.1, §12.2) -------------------------------------------------------------
// Godot-free, so render.scene times it headless on Metro with a counting sink. The lanes are threads of their own
// (bf3::core::parallel_for is a blocking fork-join and would hold the loader thread), so the loader goes on to
// load_shared_level while they run.

struct SceneLaneOptions {
	unsigned lanes = 8; // threads; the session gives the other cores to the shape cook (level_load.h)
	unsigned texture_lanes = 6; // at most this many lanes decode textures at once (decode stops scaling past ~4, gap load §2.4)
	// Prepared bytes handed to the sink and not yet released (release_bytes). A lane whose next item would cross
	// it waits; the sink is never blocked. 256 MiB [inferred]; 128 MiB is the fallback if the game's own peak
	// counts towards E6 (docs §12.4).
	uint64_t max_inflight_bytes = 256ull << 20;
};

// Receives the prepared items on the lane threads; implementations must be thread-safe and must not block (the
// Godot side queues the item and returns). Each item's bytes() stay in flight until SceneLanes::release_bytes.
class SceneSink {
public:
	virtual ~SceneSink() = default;
	virtual void mesh(PreparedSceneMesh &&p_mesh, uint64_t p_bytes) = 0;
	virtual void texture(uint32_t p_texture, PreparedTexture &&p_data, uint64_t p_bytes) = 0;
	virtual void terrain(uint32_t p_terrain, PreparedMesh &&p_mesh, uint64_t p_bytes) = 0;
	virtual void failure(const std::string &p_item, const std::string &p_error) = 0; // a missing texture never fails the load
};

struct SceneLaneStats {
	double wall_seconds = 0.0; // start -> last item handed over
	double mesh_seconds = 0.0; // summed over lanes (work time)
	double texture_seconds = 0.0;
	double terrain_seconds = 0.0;
	uint64_t mesh_items = 0, texture_items = 0, terrain_items = 0;
	uint64_t mesh_bytes = 0, texture_bytes = 0, terrain_bytes = 0; // handed to the sink
	uint64_t peak_inflight_bytes = 0;
	uint64_t cache_hits = 0, cache_converted = 0; // mesh blobs
	uint64_t failures = 0;
};

// Runs every item of a plan: mesh requests grouped by res (one blob decode, then each variant on the same lane),
// textures one item each, terrains one item each. p_plan, p_mount, p_cache and p_sink must outlive wait().
class SceneLanes {
public:
	SceneLanes();
	~SceneLanes(); // cancels and joins
	SceneLanes(const SceneLanes &) = delete;
	SceneLanes &operator=(const SceneLanes &) = delete;

	core::Status start(const LevelScenePlan &p_plan, const core::LevelMount &p_mount, core::Cache *p_cache, const SceneLaneOptions &p_options,
			SceneSink &p_sink);
	void release_bytes(uint64_t p_bytes); // any thread: the consumer freed this many prepared bytes
	void cancel(); // items not started are skipped
	bool done() const;
	core::Result<SceneLaneStats> wait(); // joins; fails only on a lane error that is not a per-item failure

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::render
