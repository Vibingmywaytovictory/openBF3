#pragma once
// The placed objects of a loaded level x mode, in Frostbite space. Ported from IW4L level_bf3
// (world.rs, variations.rs), Apache-2.0.
//
// Traversal (handoff sections/03 §2.1):
//   visit(blueprint, frame, mode, variation):
//     WorldPartReferenceObjectData: unless Excluded: visit(Blueprint, BlueprintTransform (x) frame)
//     SubWorldReferenceObjectData: if included in the mode and its bundle is loaded: a new scope
//       (and sub-world, with its own MeshVariationDatabase) over the bundle's primary SubWorldData
//     other ReferenceObjectData: unless Excluded; variation = ObjectVariation.NameHash or inherited
//       SpawnReferenceObjectData: placed itself
//       ObjectBlueprint: Object placed at Object.Transform (x) BlueprintTransform (x) frame
//       PrefabBlueprint (incl. SpatialPrefabBlueprint): a new scope, visited at that transform
//     StaticModelGroupEntityData: each member's InstanceCount instances, from InstanceTransforms or
//       else the next ShapesSubpart of the group's HavokPhysicsData res (one cursor over the
//       members), InstanceScale in `scale`, InstanceObjectVariation as the variation
//     any other object: placed at its Transform (x) frame
// Excluded, BlueprintTransform and Transform can be fed by PropertyConnections (Scopes::property).
// Depth is limited to 48 blueprint levels; fan-out by a step budget, objects to 2^20, scopes to 2^18
// and static model groups to 2^16 (a traversal past any of them fails).

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
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
	GroupMember, // an instance of a StaticModelGroupEntityData member
	Banger,
	Vehicle, // a placed VehicleEntityData (spawned vehicles are VehicleSpawnReferences)
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
	CharacterSpawnReference, // the entity is the CharacterSpawnReferenceObjectData itself
	VehicleSpawnReference, // the entity is the VehicleSpawnReferenceObjectData itself
	CapturePoint,
	CombatArea,
	VolumeVectorShape, // Points are in the frame of the placed transform
	VisualEnvironment,
	Other, // by the entity's type name
};

struct MeshObject {
	// The MeshAsset (rigid, composite or skinned); its partition name is the MeshSet res name.
	EbxInstance asset;
	MeshEntityType entity_type = MeshEntityType::Other;
	// PhysicsData of the entity (a PhysicsEntityData). Group members collide through the group's
	// Havok res instead.
	std::optional<EbxInstance> physics;

	std::string_view name() const; // MeshAsset.Name
};

// Where a static model group instance came from.
struct GroupSlot {
	uint32_t group = 0; // index into World::groups
	uint32_t member = 0;
	uint32_t instance = 0;
	std::optional<uint32_t> havok_subpart; // none when the member lists InstanceTransforms
	std::optional<uint32_t> physics_part; // first physics part, from PhysicsPartRange
};

// One placed object of the level, in Frostbite space.
struct PlacedObject {
	ObjectKind kind = ObjectKind::Other;
	std::optional<MeshObject> mesh; // kind == Mesh
	// World transform: every enclosing reference composed, interface overrides applied, a group's
	// InstanceScale in `scale`.
	LevelTransform transform;
	// The placed data: an entity data, a spawn reference, or for a group instance the member's
	// MemberType.
	EbxInstance entity;
	// The ReferenceObjectData whose ObjectBlueprint holds `entity`; none when `entity` is itself an
	// object of the scope.
	std::optional<EbxInstance> reference;
	std::optional<GroupSlot> group;
	uint32_t subworld = 0; // index into World::subworlds; its variation database textures the mesh
	uint32_t scope = 0; // index into World::scopes: the blueprint whose connections reach this object
	uint32_t variation = 0; // ObjectVariation name hash, 0 for the default look

	// The kind's name; the entity's type name for Other.
	std::string_view label() const;
};

// One sub-world's MeshVariationDatabase: entries by (MeshAsset, ObjectVariation name hash), 0 being
// the default. An entry whose Materials list is empty textures nothing and is skipped, so the mesh
// falls back to its default entry; p_keep_empty keeps such entries (IW4L before that fix, which the
// handoff's Bazaar counts were taken with).
class MeshVariationDb {
public:
	static MeshVariationDb read(const EbxResolver &p_db, const EbxInstance &p_database, bool p_keep_empty = false);

	std::optional<EbxInstance> database() const { return database_; }
	size_t size() const { return entries_.size(); }
	size_t variant_count() const;
	// Entries and RedirectEntries slots read, kept or not (what the read cost).
	size_t scanned() const { return scanned_; }
	// The MeshVariationDatabaseEntry for exactly this mesh and variation.
	std::optional<EbxInstance> entry(const EbxInstance &p_mesh, uint32_t p_variation) const;

private:
	using Key = std::tuple<Guid, Guid, uint32_t>; // mesh partition, mesh instance, variation hash
	std::optional<EbxInstance> database_;
	std::map<Key, EbxInstance> entries_;
	size_t scanned_ = 0;
};

struct SubWorld {
	std::string name; // partition of the LevelData or SubWorldData, lowercase
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
	std::optional<std::string> physics_asset; // PhysicsData.Asset.Name: the HavokPhysicsData res
	std::shared_ptr<const res::HavokPhysicsData> havok;
	size_t members = 0;
	size_t instances = 0;
	size_t explicit_count = 0; // instances placed from InstanceTransforms
	size_t havok_placed = 0; // instances placed from Havok subparts, in member order
	size_t subparts = 0; // instance transforms the Havok res holds
	// Havok-placed instances whose subpart user_data is not the first physics part
	// PhysicsPartRange gives them (0 on every retail level).
	size_t part_mismatches = 0;
};

struct WorldStats {
	size_t excluded = 0; // references skipped because they are Excluded
	size_t excluded_overrides = 0;
	size_t transform_overrides = 0;
	size_t unresolved_objects = 0; // Objects entries that do not resolve in the loaded partitions
	size_t missing_blueprints = 0;
	size_t depth_limited = 0;
	// Traversal steps taken: visits, Objects slots, group members and instances, variation database
	// slots read. Steps plus resolve_hops past 2^22 fail the traversal, which only a hostile
	// blueprint graph reaches.
	size_t steps = 0;
	size_t resolve_hops = 0; // connection hops of the property resolutions (Scopes::hops)
	std::vector<std::string> skipped_subworlds; // BundleNames the mode does not include
	std::vector<std::string> absent_subworlds; // included sub-worlds whose partition is not loaded
	size_t unnamed_subworlds = 0; // skipped or absent past the 4,096 names the lists keep
};

class World {
public:
	// Walks p_level from its LevelData. Fails when a static model group's Havok res is missing or
	// unreadable, or holds fewer subparts than its members need, or when the blueprint graph exceeds
	// the step, object or scope budget. p_level must outlive the World.
	// A World is not safe to share across threads: Scopes keeps resolution state in its const
	// methods (property, interface, and through them starts_hidden).
	static Result<std::unique_ptr<World>> traverse(const LoadedLevel &p_level);
	// The same over any partition set (p_root: the LevelData; static model group res are read
	// from p_mount). p_db and p_mount must outlive the World.
	static Result<std::unique_ptr<World>> traverse(const EbxResolver &p_db, const EbxInstance &p_root, const LevelMount &p_mount, std::string_view p_mode);

	explicit World(const EbxResolver &p_db) :
			scopes(p_db) {}

	std::vector<PlacedObject> objects;
	Scopes scopes;
	std::vector<SubWorld> subworlds; // the level itself first
	std::vector<StaticModelGroup> groups;
	WorldStats stats;

	// The placed object for p_object as a connection of p_scope names it: the entity itself or the
	// reference holding its blueprint (a group member's entity answers with its first instance).
	const PlacedObject *placed(uint32_t p_scope, const EbxInstance &p_object) const;
	// The MeshVariationDatabaseEntry that textures p_mesh placed in p_subworld: that sub-world's
	// database first, then its parents', the exact variation before the default one.
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

} // namespace bf3::core::level
