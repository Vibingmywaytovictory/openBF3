#pragma once
// The collision soup of a level x mode: every Havok collision triangle the level starts with, in
// world (Frostbite) space, with a material value, a surface class and a rigid-body kind, chunked
// into 64 m x 64 m tiles over X/Z (the tiles of the planned Jolt static world, plan §2.4 step 3).
// Godot-free. Parts ported from IW4L level_bf3 collision/mod.rs, havok.rs and material.rs
// (Apache-2.0); research/probes/tools/level-oracle `collision` is its parity oracle (the same
// placements, triangles, quantisation and hashes through IW4L's own crates).
//
// Pipeline (build_collision_soup):
//   collision_placements (census.h): each static model group's HavokPhysicsData at the group
//     transform, each non-group mesh object's PhysicsEntityData asset at its frame;
//   objects the level starts hidden (starts_hidden: ruins, backdrops) are listed, not built;
//   load_havok (res/res_loader.h) for every distinct object res, on all cores; a group reuses the
//     Havok data the traversal parsed;
//   the leaves of each root shape (IW4L havok.rs `leaves`: Mopp / List / ConvexTranslate /
//     ConvexTransform / ExtendedMesh shape subparts = group instances, then triangle subparts),
//     each through havok_triangles (convex leaves tessellated without their convex radius; mesh
//     triangles keep their stored winding, convex faces wind counter-clockwise seen from outside);
//   per triangle (IW4L havok.rs `decode`, mod.rs `class` / `intact`):
//     material = MaterialFlagsAndIndices[index]: mesh triangles by their stored index; a convex
//                leaf by its user_data bits 8..15 when in range, else MaterialIndices[first part
//                + part] (first part = the group subpart's user_data);
//     part     = the mesh chunk's part (0 in a triangle subpart), or user_data & 0xFF;
//     dropped  when the owner's PhysicsPartInfos[part].HealthStateIndex is not 0 (a destroyed
//              state's part; the owner is the object, or the group member the traversal placed at
//              the leaf's subpart, the last such member when several name one);
//     surface  = the material grid row's class (SurfaceMaterials: voted from the asset names of
//              the row's relations), else the owner mesh name's word (foliage counts as bark
//              unless see-through), else concrete;
//     body     = RigidBodies[root].RigidBodyType of the object (groups: collision);
//     flags    = see-through (material bit 27) | invisible blocker (mesh name has "invisible" or
//              "collisionmesh").
//   then the terrain (build_terrain below): each TerrainEntityData's heightfield (res/terrain.h)
//     on a grid of about SoupOptions::terrain_spacing, the cells around the Havok tiles as two
//     triangles each;
//   Unlike IW4L's IW soup nothing is welded, re-wound, cropped or made into brushes, and foliage
//   is kept (its surface tags it): this is what a Jolt static world and a debug view start from.
//   Every coordinate is quantised to whole millimetres for the hashes and the tile key; a triangle
//   with a non-finite or |c| > 1e7 m coordinate is dropped (nonfinite), and one with two equal
//   quantised corners (degenerate). The kept triangle stores its float corners.
//   Tile of a triangle: floor((qx0 + qx1 + qx2) / 192000) and the same over Z (its centroid in
//   whole 64 m tiles); triangles are not clipped at tile borders.
//
// Hashes (FNV-1a 64; the oracle prints the same): a triangle's bytes are its nine millimetre
// values as little-endian i64, the material as little-endian u32, then surface, body and flags.
// A placement's hash runs over its triangles in walk order; a tile's and the soup's `sum` are the
// wrapping u64 sums of their triangles' own hashes (order-free).
//
// Bounds: the soup's EBX reads (placements, material grid, rigid bodies, part infos, names) charge
// the World's WorkBudget like the other helpers; the triangle output is bounded by the Havok
// decoders' own budgets (shape tree size and hull work per packfile byte, res/havok.h), and the
// whole soup by kMaxSoupTriangles. Hostile data fails with an error; a res that does not load
// marks its placement Missing and the soup goes on.

#include "bf3/core/install.h"
#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/havok.h"
#include "bf3/core/result.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::level {

// IW4L collision/material.rs surface types (IW `surface_type`, 0..31); 0 is unused here.
namespace surface {
inline constexpr uint8_t kBark = 1;
inline constexpr uint8_t kBrick = 2;
inline constexpr uint8_t kCarpet = 3;
inline constexpr uint8_t kCloth = 4;
inline constexpr uint8_t kConcrete = 5;
inline constexpr uint8_t kDirt = 6;
inline constexpr uint8_t kFlesh = 7;
inline constexpr uint8_t kFoliage = 8;
inline constexpr uint8_t kGlass = 9;
inline constexpr uint8_t kGrass = 10;
inline constexpr uint8_t kGravel = 11;
inline constexpr uint8_t kIce = 12;
inline constexpr uint8_t kMetal = 13;
inline constexpr uint8_t kMud = 14;
inline constexpr uint8_t kPaper = 15;
inline constexpr uint8_t kPlaster = 16;
inline constexpr uint8_t kRock = 17;
inline constexpr uint8_t kSand = 18;
inline constexpr uint8_t kSnow = 19;
inline constexpr uint8_t kWater = 20;
inline constexpr uint8_t kWood = 21;
inline constexpr uint8_t kAsphalt = 22;
inline constexpr uint8_t kCeramic = 23;
inline constexpr uint8_t kPlastic = 24;
inline constexpr uint8_t kRubber = 25;
inline constexpr uint8_t kCushion = 26;
inline constexpr uint8_t kFruit = 27;
inline constexpr uint8_t kPaintedMetal = 28;
inline constexpr uint8_t kSlush = 30;
} // namespace surface
inline constexpr size_t kSurfaceCount = 32;
// "concrete", "metal", ...; "none" for an unused id.
std::string_view surface_name(uint8_t p_surface);
// The surface an asset or object name points at (IW4L material.rs `classify`): the first word of
// the table (concrete first) that one of the name's letter tokens holds; words of four letters or
// fewer must start the token. nullopt when no word matches.
std::optional<uint8_t> classify_surface(std::string_view p_name);

// MaterialFlagsAndIndices: flags in the top byte, the low 24 bits index MaterialGridData.MaterialPairs.
inline constexpr uint32_t kMaterialSeeThrough = 0x0800'0000;
inline constexpr uint32_t kMaterialPairMask = 0x00FF'FFFF;

// The level's MaterialGridData read as surface classes (port of IW4L material.rs Materials): a
// material value names a pair, the pair's PhysicsMaterialIndex indexes MaterialIndexMap, which
// gives the grid row; a row has no name, so its surface is voted from the asset names (impact
// effects 3, generic-material and special-object sounds 2, anything else 1) that the
// interaction relations of its items reference. The default row names no surface.
class SurfaceMaterials {
public:
	enum class Lookup : uint8_t {
		Surface,
		Unnamed, // the default row, or a row whose assets name no material
		Unknown, // a pair or row the loaded partitions do not resolve
	};

	// Grid through p_root's RuntimeMaterialGrid; an empty table when there is none. Charges every
	// list it scans and every name it classifies to r_budget.
	static Result<SurfaceMaterials> read(const EbxResolver &p_db, const EbxInstance &p_root, WorkBudget &r_budget);

	size_t pair_count() const { return pairs_.size(); }
	size_t resolved_pairs() const;
	size_t row_count() const { return rows_.size(); }
	const std::vector<std::optional<uint8_t>> &row_surfaces() const { return rows_; }
	// The surface of a material value; r_surface is set for Lookup::Surface.
	Lookup lookup(uint32_t p_value, uint8_t &r_surface) const;

private:
	std::vector<std::optional<uint8_t>> pairs_;
	std::vector<uint32_t> index_map_;
	std::vector<std::optional<uint8_t>> rows_;
};

// RigidBodies[].RigidBodyType: RBTypeRaycast, RBTypeCharacter, anything else = collision.
enum class CollisionBody : uint8_t { Collision = 0, Raycast = 1, Character = 2 };
inline constexpr size_t kCollisionBodyCount = 3;
std::string_view collision_body_name(uint8_t p_body);
inline constexpr uint8_t kSoupSeeThrough = 1;
inline constexpr uint8_t kSoupInvisible = 2;
inline constexpr uint8_t kSoupTerrain = 4; // a terrain heightfield cell (material 0, surface dirt)

struct SoupTriangle {
	std::array<res::Vec3, 3> v{}; // world space, Frostbite metres
	uint32_t material = 0; // the MaterialFlagsAndIndices value
	uint32_t placement = 0; // index into CollisionSoup::placements; terrain t: placements.size() + t
	uint8_t surface = 0;
	uint8_t body = 0; // CollisionBody
	uint8_t flags = 0; // kSoupSeeThrough | kSoupInvisible | kSoupTerrain
};

struct SoupPlacement {
	enum class State : uint8_t {
		Built,
		Hidden, // the level starts the object hidden
		Missing, // the res is not in the bundles or did not decode (error says why)
		NoRes, // the physics data names no asset
	};
	CollisionPlacement::Kind kind = CollisionPlacement::Kind::Group;
	uint32_t index = 0; // World::groups or World::objects
	std::string res;
	State state = State::Built;
	std::string error;
	size_t triangles = 0;
	size_t destroyed = 0; // triangles of a part some later health state shows
	size_t degenerate = 0;
	size_t nonfinite = 0;
	uint64_t hash = 0; // FNV-1a over its triangles (see the header comment)
};

// One TerrainEntityData's heightfield in the soup. The grid is the streaming tree's height_grid at
// the first quadtree depth whose sample spacing reaches SoupOptions::terrain_spacing (+0.1%), short
// of the tree's deepest level and of res::kMaxGridSide (IW4L collision/terrain.rs; without its
// near-area cap, block merging and paved decals). Kept cells: those whose centre (in f64, grid
// coordinates) falls in a 64 m tile that holds a Havok triangle or is next to one (8-neighbours),
// none of whose four samples is a hole. A cell with corners a = (x, z), b = (x + 1, z),
// c = (x, z + 1), d = (x + 1, z + 1) gives (a, d, b) and (a, c, d) at the object's frame, each wound
// to face +Y; rows along +Z, cells along +X. Material 0, surface dirt, body collision, kSoupTerrain.
struct SoupTerrain {
	enum class State : uint8_t {
		Built,
		Missing, // the res or its streaming tree is missing or does not decode (error says why)
		NoRes, // TerrainAsset names nothing
	};
	uint32_t object = 0; // World::objects
	std::string asset;
	State state = State::Built;
	std::string error;
	uint8_t depth = 0; // height_grid depth
	size_t side = 0; // grid samples a side
	size_t holes = 0; // hole samples in the whole grid
	size_t cells = 0; // cells in the kept tiles
	size_t open = 0; // of those, left open (a hole corner)
	size_t triangles = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	uint64_t hash = 0; // FNV-1a over its triangles in emit order
};

struct SoupTile {
	int32_t x = 0; // floor(centroid x / 64 m)
	int32_t z = 0;
	std::vector<SoupTriangle> triangles; // placement order, then walk order; terrain last
	uint64_t sum = 0; // wrapping sum of the triangle hashes
	res::Vec3 min{}; // bounds of the triangles' corners
	res::Vec3 max{};
};

// Loader progress the Godot label polls; stage names come from soup_stage_name.
struct SoupProgress {
	enum Stage : int { Mount = 0, Traverse, Placements, Materials, Decode, Triangles, Terrain, Tiles, Done, kStages };
	std::atomic<int> stage{ Mount };
	std::atomic<size_t> done{ 0 };
	std::atomic<size_t> total{ 0 };
};
std::string_view soup_stage_name(int p_stage);

struct SoupOptions {
	float tile_size = 64.0f; // informational: the tile key is fixed at 64 m (the hashes depend on it)
	unsigned threads = 0; // 0 = all cores
	bool terrain = true; // add the terrain heightfields
	float terrain_spacing = 2.0f; // target metres between terrain samples
	SoupProgress *progress = nullptr;
};

// The soup algorithm's version: bump it whenever the same install gives different soup triangles, tags or
// tiles (terrain handling, body / layer split, destroyed-part rule, quantisation). Derived caches built from the
// soup without hashing its triangles (bf3_sim's "jolt-world" entries) put it in their keys (docs/design/m1.md
// §5.5).
inline constexpr uint32_t kCollisionSoupVersion = 1;

// Retail Metro TDM holds 1.11 M triangles (0.17 M of them terrain); a soup past this fails (each triangle costs 48 bytes).
inline constexpr size_t kMaxSoupTriangles = size_t(1) << 25;

struct CollisionSoup {
	std::vector<SoupPlacement> placements; // collision_placements order (groups, then objects)
	std::vector<SoupTerrain> terrains; // TerrainEntityData objects, object order
	std::vector<SoupTile> tiles; // sorted by (x, z)
	size_t triangles = 0; // terrain included
	size_t terrain_triangles = 0;
	size_t destroyed = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	size_t built = 0;
	size_t hidden = 0;
	size_t missing = 0;
	size_t no_res = 0;
	uint64_t sum = 0;
	std::array<size_t, kSurfaceCount> surfaces{};
	std::array<size_t, kCollisionBodyCount> bodies{};
	res::Vec3 min{}; // bounds of every kept corner (zero when empty)
	res::Vec3 max{};
	size_t res_decoded = 0; // distinct object res loaded + group res whose shapes were decoded
	size_t material_pairs = 0;
	size_t resolved_pairs = 0;
	double seconds_placements = 0.0; // placements, materials, owners (calling thread)
	double seconds_decode = 0.0; // load_havok + group shapes (all cores)
	double seconds_triangles = 0.0; // leaves, triangles, classes, hashes (all cores)
	double seconds_terrain = 0.0; // height grids and terrain triangles
	double seconds_tiles = 0.0;
};

// Builds the soup of a traversed level (p_world from World::traverse(p_level)). Reads the install
// through p_level's mount only (read-only). Uses up to p_options.threads lanes; the World is only
// touched from the calling thread. Fails on an exhausted work budget or past kMaxSoupTriangles.
Result<CollisionSoup> build_collision_soup(const LoadedLevel &p_level, const World &p_world, const SoupOptions &p_options = {});

// The pieces the oracle defines, for tests: quantise a triangle (false = non-finite or out of
// range), whether two quantised corners coincide, its hash bytes' FNV-1a, its tile.
bool soup_quantise(const std::array<res::Vec3, 3> &p_v, std::array<int64_t, 9> &r_q);
bool soup_degenerate(const std::array<int64_t, 9> &p_q);
void soup_hash_bytes(uint64_t &r_hash, const std::array<int64_t, 9> &p_q, uint32_t p_material, uint8_t p_surface, uint8_t p_body, uint8_t p_flags);
inline constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
std::array<int32_t, 2> soup_tile_of(const std::array<int64_t, 9> &p_q);

// What build_collision_soup does for one placement, over shapes already decoded: the leaves of
// every root at p_frame, root i being body p_bodies[i] (collision past the end), every leaf owned
// by one mesh named p_owner_name whose parts are all intact. For tests and tools.
struct SoupShapeResult {
	std::vector<SoupTriangle> triangles; // placement 0
	std::vector<std::array<int32_t, 2>> tiles; // per triangle
	size_t destroyed = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	uint64_t hash = 0; // the placement hash
	uint64_t sum = 0; // wrapping sum of the triangle hashes
};
SoupShapeResult soup_shape_triangles(const res::HavokPhysicsData &p_havok, const std::vector<res::Shape> &p_roots, const res::Transform &p_frame,
		const std::vector<uint8_t> &p_bodies, const SurfaceMaterials &p_materials, std::string_view p_owner_name);

// Load, traverse and build in one call (the Godot overlay's loader thread and the tests). Members
// are declared so that the World goes before the level it points into.
struct LevelCollision {
	std::unique_ptr<LoadedLevel> level;
	std::unique_ptr<World> world;
	CollisionSoup soup;
	std::vector<LevelTransform> spawns; // AlternateSpawn placements (TDM spawn points), object order
	double seconds_load = 0.0;
	double seconds_traverse = 0.0;
	double seconds_soup = 0.0;
};
Result<LevelCollision> load_level_collision(std::shared_ptr<const Install> p_install, std::string_view p_level_id, std::string_view p_mode,
		const SoupOptions &p_options = {});

} // namespace bf3::core::level
