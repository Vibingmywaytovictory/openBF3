#pragma once

#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/res_math.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core::level {

Result<std::optional<std::vector<uint16_t>>> hidden_parts(const EbxResolver &p_db, const EbxInstance &p_entity, WorkBudget &r_budget);

Result<bool> starts_hidden(const World &p_world, const PlacedObject &p_object);

struct BakeSplit {
	LevelTransform placed;
	std::optional<std::array<std::array<float, 3>, 3>> linear;
};
BakeSplit bake_split(const LevelTransform &p_transform);
std::array<int32_t, 9> bake_key(const std::array<std::array<float, 3>, 3> &p_linear);

struct CensusOptions {
	bool legacy_variations = false;
	unsigned threads = 0;
};

struct SceneMesh {
	EbxInstance asset;
	std::string name;
	std::optional<EbxInstance> entry;
	std::vector<uint16_t> hidden;
	std::optional<std::array<std::array<float, 3>, 3>> bake;
	std::array<int32_t, 9> bake_key{};
	size_t placements = 0;
	float radius = 0.0f;
	bool world_kind = false;
	std::vector<bool> lods;
	bool kept = false;
	bool failed = false;
};

struct SceneInstance {
	uint32_t mesh = 0;
	uint32_t object = 0;
	LevelTransform transform;
};

struct MeshCensus {
	std::vector<SceneMesh> meshes;
	std::vector<SceneInstance> instances;
	std::vector<std::pair<std::string, std::string>> failures;
	size_t placements = 0;
	size_t scene_meshes = 0;
	size_t model_meshes = 0;
	size_t world_meshes = 0;
	size_t scene_instances = 0;
};

Result<MeshCensus> mesh_census(const LoadedLevel &p_level, const World &p_world, const CensusOptions &p_options = {});
Result<MeshCensus> mesh_census(const EbxResolver &p_db, const LevelMount &p_mount, const World &p_world, const CensusOptions &p_options = {});

struct CollisionPlacement {
	enum class Kind : uint8_t { Group, Object };
	Kind kind = Kind::Group;
	uint32_t index = 0;
	std::string res;
	bool present = false;
	float asset_scale = 1.0f;
	size_t subparts = 0;
	std::vector<std::string_view> bodies;

	res::Transform frame;
};

Result<std::vector<CollisionPlacement>> collision_placements(const LoadedLevel &p_level, const World &p_world);
Result<std::vector<CollisionPlacement>> collision_placements(const EbxResolver &p_db, const LevelMount &p_mount, const World &p_world);

}
