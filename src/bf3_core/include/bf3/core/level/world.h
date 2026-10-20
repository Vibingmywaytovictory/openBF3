#pragma once

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/level/connections.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/res/havok.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace bf3::core::level {

enum class MeshEntityType : uint8_t {
	StaticModel,
	MeshProxy,
	Breakable,
	Dynamic,
	VegetationTree,
	GroupMember,
	Banger,
	Vehicle,
	Other,
};
std::string_view mesh_entity_type_name(MeshEntityType p_type);

enum class ObjectKind : uint8_t {
	Mesh,
	Terrain,
	Water,
	Light,
	Decal,
	Effect,
	AlternateSpawn,
	CharacterSpawnReference,
	VehicleSpawnReference,
	CapturePoint,
	CombatArea,
	VolumeVectorShape,
	VisualEnvironment,
	Other,
};

struct MeshObject {

	EbxInstance asset;
	MeshEntityType entity_type = MeshEntityType::Other;

	std::optional<EbxInstance> physics;

	Result<std::string_view> name(WorkBudget &r_budget) const;
};

struct GroupSlot {
	uint32_t group = 0;
	uint32_t member = 0;
	uint32_t instance = 0;
	std::optional<uint32_t> havok_subpart;
	std::optional<uint32_t> physics_part;
};

struct PlacedObject {
	ObjectKind kind = ObjectKind::Other;
	std::optional<MeshObject> mesh;

	LevelTransform transform;

	EbxInstance entity;

	std::optional<EbxInstance> reference;
	std::optional<GroupSlot> group;
	uint32_t subworld = 0;
	uint32_t scope = 0;
	uint32_t variation = 0;

	std::string_view label() const;
};

class MeshVariationDb {
public:

	static Result<MeshVariationDb> read(const EbxResolver &p_db, const EbxInstance &p_database, WorkBudget &r_budget, bool p_keep_empty = false);

	std::optional<EbxInstance> database() const { return database_; }
	size_t size() const { return entries_.size(); }
	size_t variant_count() const;

	size_t scanned() const { return scanned_; }

	std::optional<EbxInstance> entry(const EbxInstance &p_mesh, uint32_t p_variation) const;

private:
	using Key = std::tuple<Guid, Guid, uint32_t>;
	std::optional<EbxInstance> database_;
	std::map<Key, EbxInstance> entries_;
	size_t scanned_ = 0;
};

struct SubWorld {
	std::string name;
	EbxInstance data;
	std::optional<EbxInstance> reference;
	std::optional<uint32_t> parent;
	uint32_t scope = 0;
	MeshVariationDb variations;
};

struct StaticModelGroup {
	EbxInstance entity;
	uint32_t subworld = 0;
	uint32_t scope = 0;
	LevelTransform transform;
	std::optional<std::string> physics_asset;
	std::shared_ptr<const res::HavokPhysicsData> havok;
	size_t members = 0;
	size_t instances = 0;
	size_t explicit_count = 0;
	size_t havok_placed = 0;
	size_t subparts = 0;

	size_t part_mismatches = 0;
};

struct WorldStats {
	size_t excluded = 0;
	size_t excluded_overrides = 0;
	size_t transform_overrides = 0;
	size_t unresolved_objects = 0;
	size_t missing_blueprints = 0;
	size_t depth_limited = 0;

	size_t steps = 0;
	size_t resolve_hops = 0;
	size_t capped_resolutions = 0;
	size_t work = 0;
	std::vector<std::string> skipped_subworlds;
	std::vector<std::string> absent_subworlds;
	size_t unnamed_subworlds = 0;
};

class World {
public:

	static Result<std::unique_ptr<World>> traverse(const LoadedLevel &p_level);

	static Result<std::unique_ptr<World>> traverse(const EbxResolver &p_db, const EbxInstance &p_root, const LevelMount &p_mount, std::string_view p_mode,
			std::shared_ptr<WorkBudget> p_budget = nullptr);

	World(const EbxResolver &p_db, std::shared_ptr<WorkBudget> p_budget) :
			budget(std::move(p_budget)), scopes(p_db, *budget) {}

	std::shared_ptr<WorkBudget> budget;
	std::vector<PlacedObject> objects;
	Scopes scopes;
	std::vector<SubWorld> subworlds;
	std::vector<StaticModelGroup> groups;
	WorldStats stats;

	const PlacedObject *placed(uint32_t p_scope, const EbxInstance &p_object) const;

	std::optional<EbxInstance> mesh_variation(uint32_t p_subworld, const EbxInstance &p_mesh, uint32_t p_variation) const;

private:
	friend class Walker;
	struct PlacedKey {
		uint32_t scope;
		InstanceKey instance;
		bool operator==(const PlacedKey &) const = default;
	};
	struct PlacedKeyHash {
		size_t operator()(const PlacedKey &p_key) const noexcept { return InstanceKeyHash()(p_key.instance) ^ (size_t(p_key.scope) * 31u); }
	};
	std::unordered_map<PlacedKey, uint32_t, PlacedKeyHash> placed_;
};

}
