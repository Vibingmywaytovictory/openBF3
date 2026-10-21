#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/math.h"
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
}
namespace bf3::core::level {
class LoadedLevel;
class World;
struct MeshCensus;
}

namespace bf3::render {

struct SceneMeshRequest {
	std::string res;
	MeshRequest request;
	std::vector<uint32_t> variants;
};
std::vector<SceneMeshRequest> scene_mesh_requests(const core::level::MeshCensus &p_census);

enum class SceneLook : uint8_t {
	M1,
	L1,
};

struct ScenePlanOptions {
	bool include_hidden_start = false;
	uint32_t max_lod = 0;
	bool include_terrain = true;
	SceneLook look = SceneLook::L1;

	const core::LevelMount *mount = nullptr;
};

struct SceneTerrainPlan {
	uint32_t object = 0;
	std::string asset;
	core::level::LevelTransform transform;
	std::array<float, 3> color{ 0.36f, 0.33f, 0.27f };
};
struct SceneTerrainOptions {
	float spacing = 4.0f;
};

struct SceneTexturePlan {
	std::string name;
	TextureRole role = TextureRole::Color;
};

struct SceneMaterialPlan {
	MaterialSlot slot;
	std::vector<int32_t> textures;

	int32_t albedo = -1;
	int32_t normal = -1;
	bool alpha_test = false;
	bool double_sided = false;

	uint8_t albedo_uv = kLookUv0;
	float albedo_scale = 1.0f;
	uint8_t normal_uv = kLookUv0;
	float normal_scale = 1.0f;
	bool blend = false;
	bool glass = false;
	std::optional<LookMaterial> look;
};

inline constexpr uint32_t kSceneUndrawnMaterial = 0xFFFFFFFFu;

struct SceneMeshPlan {
	uint32_t request = 0;
	uint32_t census_mesh = 0;
	std::vector<uint16_t> hidden_parts;
	std::optional<std::array<core::Vec3, 3>> bake;
	std::vector<uint32_t> surface_materials;
	bool world_kind = false;
	uint32_t instances = 0;
};

struct SceneInstancePlan {
	uint32_t mesh = 0;
	core::level::LevelTransform transform;
};

struct SceneEnvironment {
	bool from_data = false;
	core::Vec3 sun_direction{ -0.35f, -0.8f, 0.45f };
	core::Vec3 sun_color{ 1.0f, 0.96f, 0.9f };
	float sun_energy = 1.0f;
	core::Vec3 sky_color{ 0.45f, 0.55f, 0.7f };
	core::Vec3 ground_color{ 0.25f, 0.24f, 0.22f };
	float ambient_energy = 0.6f;
	float fog_start = 0.0f;
	float fog_end = 0.0f;
	core::Vec3 fog_color{ 0.6f, 0.65f, 0.7f };
};

struct LevelScenePlan {
	std::vector<SceneMeshRequest> requests;
	std::vector<SceneMeshPlan> meshes;
	std::vector<SceneInstancePlan> instances;
	std::vector<SceneMaterialPlan> materials;
	std::vector<SceneTexturePlan> textures;
	std::vector<SceneTerrainPlan> terrains;
	SceneEnvironment environment;
	std::vector<std::pair<std::string, std::string>> failures;
	double seconds = 0.0;
	SceneLook look = SceneLook::L1;
	LookStats look_stats;

	LookEnvironment look_environment;
	int32_t sky_panorama = -1, sky_panorama_alpha = -1, sky_gradient = -1;
};

core::Result<LevelScenePlan> plan_level_scene(const core::EbxResolver &p_resolver, const core::level::World &p_world,
		const core::level::MeshCensus &p_census, const ScenePlanOptions &p_options = {});
core::Result<LevelScenePlan> plan_level_scene(const core::level::LoadedLevel &p_level, const core::level::World &p_world,
		const core::level::MeshCensus &p_census, const ScenePlanOptions &p_options = {});

struct PreparedSceneMesh {
	uint32_t mesh = 0;
	PreparedMesh lod0;
	core::CachedItem item;
};

core::Result<PreparedSceneMesh> prepare_scene_mesh(core::Cache *p_cache, const core::LevelMount &p_mount, const LevelScenePlan &p_plan,
		uint32_t p_mesh, const PreparedMeshSet &p_blob);

core::Result<CachedMesh> load_scene_request(core::Cache *p_cache, const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_request);

core::Result<PreparedTexture> prepare_scene_texture(const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_texture);

core::Result<PreparedMesh> prepare_scene_terrain(const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_terrain,
		const SceneTerrainOptions &p_options = {});

uint64_t estimate_scene_bytes(const LevelScenePlan &p_plan, const core::LevelMount &p_mount);

struct SceneLaneOptions {
	unsigned lanes = 8;
	unsigned texture_lanes = 6;

	uint64_t max_inflight_bytes = 256ull << 20;
};

class SceneSink {
public:
	virtual ~SceneSink() = default;
	virtual void mesh(PreparedSceneMesh &&p_mesh, uint64_t p_bytes) = 0;
	virtual void texture(uint32_t p_texture, PreparedTexture &&p_data, uint64_t p_bytes) = 0;
	virtual void terrain(uint32_t p_terrain, PreparedMesh &&p_mesh, uint64_t p_bytes) = 0;
	virtual void failure(const std::string &p_item, const std::string &p_error) = 0;
};

struct SceneLaneStats {
	double wall_seconds = 0.0;
	double mesh_seconds = 0.0;
	double texture_seconds = 0.0;
	double terrain_seconds = 0.0;
	uint64_t mesh_items = 0, texture_items = 0, terrain_items = 0;
	uint64_t mesh_bytes = 0, texture_bytes = 0, terrain_bytes = 0;
	uint64_t peak_inflight_bytes = 0;
	uint64_t cache_hits = 0, cache_converted = 0;
	uint64_t failures = 0;
};

class SceneLanes {
public:
	SceneLanes();
	~SceneLanes();
	SceneLanes(const SceneLanes &) = delete;
	SceneLanes &operator=(const SceneLanes &) = delete;

	core::Status start(const LevelScenePlan &p_plan, const core::LevelMount &p_mount, core::Cache *p_cache, const SceneLaneOptions &p_options,
			SceneSink &p_sink);
	void release_bytes(uint64_t p_bytes);
	void cancel();
	bool done() const;
	core::Result<SceneLaneStats> wait();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
