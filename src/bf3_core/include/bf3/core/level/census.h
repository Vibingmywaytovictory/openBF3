#pragma once
// What a traversed level places: the scene meshes and instances its mesh placements need, the
// destruction parts and start-hidden placements, and the collision placements (which Havok res sits
// where). Counting parts of IW4L level_bf3 geometry/mod.rs, geometry/meshes.rs and collision/mod.rs
// (Apache-2.0); Frostbite space throughout.
//
// Mesh census (the IW4L geometry builder's definition when the handoff §9 Metro and Bazaar counts
// were taken, IW4L bab7c1a):
//   * a scene mesh per (MeshAsset, MeshVariationDatabase entry, hidden composite parts, bake);
//     the bake is the part of a placement a uniformly scaled rotation cannot carry (non-uniform
//     scale or shear: 2.6 % of Metro's placements), keyed by its 3x3 matrix rounded to 1/4096;
//   * hidden parts: a CompositeMeshAsset placement hides the parts that only later health states
//     of its PartComponentData show (HealthStates[1..].PartIndex minus HealthStates[0]);
//   * a mesh draws when one of its first 4 LODs (LOD0, or LOD1 when LOD0 draws nothing) has a
//     triangle that survives: non-ZOnly subset, a material whose shader partition name has none of
//     invisible / occluder / shadowmesh / ui_characterbackdrop / ui_menu, no corner in a hidden
//     part, not degenerate. A mesh that draws nothing has no instance (Metro's 304
//     invisiblecharactercollision placements);
//   * world kind: placed at most 4 times with a placed bounding radius of at least 16 m (it then
//     keeps only LOD0); the rest are model kind;
//   * terrain and water objects add one world mesh and one instance each.
// Metro TDM: 10,873 placements -> 842 meshes (762 model, 80 world), 10,571 instances, exactly the
// handoff. Bazaar CQL gives 613 meshes / 5,783 instances; the handoff's 620 is reproduced with
// legacy_variations (IW4L before 066d34c, 2026-10-07, still used an entry with an empty Materials
// list).
// Every function here charges the World's WorkBudget (or the one given) before each list it scans
// (cost model in src/level/world.cpp) and fails with the budget error once it is exhausted.

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

// The parts of p_entity's composite mesh its first health state does not show, sorted; none when
// the entity has no PartComponentData (every part drawn). At most 4,096 components are walked.
Result<std::optional<std::vector<uint16_t>>> hidden_parts(const EbxResolver &p_db, const EbxInstance &p_entity, WorkBudget &r_budget);

// Whether the placement starts invisible: its reference's Visible is fed by a BoolEntityData whose
// DefaultValue is false (destruction ruins, Metro's backdrop_park / backdrop_city prefabs).
Result<bool> starts_hidden(const World &p_world, const PlacedObject &p_object);

// A uniformly scaled rotation the instance carries, plus the 3x3 linear map baked into the mesh
// (rows: baked = linear * p) when the transform is not one; IW4L geometry/meshes.rs Bake::split.
struct BakeSplit {
	LevelTransform placed;
	std::optional<std::array<std::array<float, 3>, 3>> linear;
};
BakeSplit bake_split(const LevelTransform &p_transform);
std::array<int32_t, 9> bake_key(const std::array<std::array<float, 3>, 3> &p_linear);

struct CensusOptions {
	bool legacy_variations = false; // see the header comment and MeshVariationDb::read
	unsigned threads = 0; // MeshSet decoding lanes; 0 = all cores
};

struct SceneMesh {
	EbxInstance asset;
	std::string name; // MeshSet res = MeshAsset partition name
	std::optional<EbxInstance> entry; // MeshVariationDatabaseEntry
	std::vector<uint16_t> hidden; // composite parts left out, sorted
	std::optional<std::array<std::array<float, 3>, 3>> bake;
	std::array<int32_t, 9> bake_key{};
	size_t placements = 0;
	float radius = 0.0f; // placed bounding radius, metres (mesh radius x largest placed scale)
	bool world_kind = false;
	std::vector<bool> lods; // per decoded LOD (at most 4): whether it draws a triangle
	bool kept = false; // a scene mesh with instances
	bool failed = false; // the MeshSet did not read or decode
};

struct SceneInstance {
	uint32_t mesh = 0; // index into MeshCensus::meshes
	uint32_t object = 0; // index into World::objects
	LevelTransform transform; // unit-scaled rotation + uniform scale + translation (bake applied to the mesh)
};

struct MeshCensus {
	std::vector<SceneMesh> meshes; // every variant, kept or not, in first-placement order
	std::vector<SceneInstance> instances; // one per placement of a kept mesh
	std::vector<std::pair<std::string, std::string>> failures; // MeshSets that did not read: (name, error)
	size_t placements = 0;
	size_t scene_meshes = 0; // kept variants + terrain + water
	size_t model_meshes = 0;
	size_t world_meshes = 0;
	size_t scene_instances = 0; // kept placements + terrain + water
};

// Reads (read-only) and decodes every placed MeshSet of the level. A mesh that does not decode is
// listed in failures and has no instance; only an exhausted work budget fails the census.
Result<MeshCensus> mesh_census(const LoadedLevel &p_level, const World &p_world, const CensusOptions &p_options = {});
Result<MeshCensus> mesh_census(const EbxResolver &p_db, const LevelMount &p_mount, const World &p_world, const CensusOptions &p_options = {});

// One collision placement: a static model group's Havok res at the group transform (instances at
// the res's ShapesSubparts), or a placed mesh object's PhysicsEntityData asset.
struct CollisionPlacement {
	enum class Kind : uint8_t { Group, Object };
	Kind kind = Kind::Group;
	uint32_t index = 0; // Group: into World::groups; Object: into World::objects
	std::string res; // HavokPhysicsData res name; empty when the physics data names no asset
	bool present = false; // the level's bundles carry the res
	float asset_scale = 1.0f; // Object: the Scale the chosen asset was prebaked at
	size_t subparts = 0; // Group
	std::vector<std::string_view> bodies; // Object: RigidBodies[].RigidBodyType
	// Where the res's shapes go: the group transform, or for an object its axes times placement
	// scale over asset_scale (IW4L collision frame_of), origin = translation.
	res::Transform frame;
};

// Groups with a physics asset, then every non-group mesh object with PhysicsData, in object order.
// Objects take the asset of ScaledAssets/Asset prebaked closest to the placement's scale that the
// bundles carry (IW4L collision pick_asset); an asset name over 1,024 bytes (no res has one) is
// skipped. Fails only on an exhausted work budget.
Result<std::vector<CollisionPlacement>> collision_placements(const LoadedLevel &p_level, const World &p_world);
Result<std::vector<CollisionPlacement>> collision_placements(const EbxResolver &p_db, const LevelMount &p_mount, const World &p_world);

} // namespace bf3::core::level
