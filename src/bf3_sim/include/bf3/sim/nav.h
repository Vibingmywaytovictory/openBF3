#pragma once
// Infantry navigation: a tiled Recast/Detour navmesh baked from exactly the triangles the soldier motor collides
// with (spike S1; plan §3.6, D11, R6, §3.12; research topics/06 §2.4-§2.6, gap load §2.7; docs/design/s1.md §5).
// Godot-free; Recast and Detour stay PRIVATE to bf3_sim (this header names no Recast or Detour type).
//
// Input set (docs §5.1): the collision soup's triangles whose layer (static_layer_of, the one rule the static world's
// cook uses) is in query_mask::kMovement = Collision | Character: every Collision body (static model groups, meshes'
// RBTypeCollision bodies, the terrain cells, invisible blockers), every Character body (PLAYERCLIP); never a
// Raycast body (railings, foliage shells, see-through detail), never a part only a later health state shows (the
// soup drops those), never an object the level starts hidden (the soup lists, does not build them). Terrain holes
// are already open in the soup. Foliage and scatter have no Havok collision in the soup unless their body is
// Collision, in which case the motor collides with them and so does the nav. With SoupOptions::terrain_cover the
// soup keeps the terrain of every combat-area tile (the motor's static world uses the same cover).
//
// Bake (docs §5.3): 64 m tiles on the soup's tile grid (tile (x, z) covers [64x, 64x + 64) x [64z, 64z + 64)), cs
// 0.25, ch 0.1, the soldier from MotorParams (radius 0.35, stand height 1.8, crouch height 1.15, climb = stand
// StepHeight 0.3, slope = MaxAscendAngle 50; verified with bf3dump soldier on MP_Subway), over the given tiles only
// (the union of the level's MP modes' combat-area tiles + margin, mode_areas.h). Each tile rasterises its own and
// its neighbours' input triangles within the border (walkable radius + 3 cells), so tiles are independent and bake
// on parallel_for lanes; walkable height is the crouch height and spans with less than the stand height of room are
// area kNavAreaCrouch. Drop-down off-mesh links (one way) are found per tile from its own input (border included)
// and checked with a stand-capsule sweep. Detour tiles are assembled into one dtNavMesh (64-bit poly refs,
// RECASTNAVIGATION_DT_POLYREF64; Kharg has 1,494 combat-area tiles in CQL alone and dense tiles may need more than
// the 1,024 polys a 32-bit ref leaves at 12 tile bits).
//
// Cache (bf3_core Cache, docs §5.4): "nav-tile" (one Detour tile's data + its NavTileInfo; params = the SHA-256 of
// the tile's input triangles incl. its border, nav_params_text, recast_build_id), "nav-world" (the tile list of a
// level x mode: params = the static world's source digest, the soup version, the terrain cover and the region
// digests, nav_params_text), "nav-area" (the union of a level's combat-area tiles, keyed by the source digests of
// every mode). Warm: nav-world -> nav-tile blobs -> validate_detour_tile -> addTile; no soup, no Recast.
// Threads: a NavWorld is immutable once built and shared; Detour's dtNavMesh is read-only in queries, so any number
// of threads query it at once, each through its own NavQuery (nav_query.h).

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/static_world.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Cache;
}
namespace bf3::core::level {
struct CollisionSoup;
class LoadedLevel;
class World;
} // namespace bf3::core::level

namespace bf3::sim {

class CollisionWorld;
struct MotorParams;

// Detour poly reference (dtPolyRef with DT_POLYREF64; nav.cpp static_asserts the match). 0 = none.
using NavPolyRef = uint64_t;

inline constexpr float kNavTileSize = 64.0f; // = the soup's and the static world's tile
// Area ids (Detour areas 0..63) and poly flags (dtQueryFilter include/exclude masks).
inline constexpr uint8_t kNavAreaWalk = 1; // stand height of room
inline constexpr uint8_t kNavAreaCrouch = 2; // between the crouch and the stand height of room: the walker crouches
inline constexpr uint8_t kNavAreaDrop = 3; // a drop-down off-mesh link
namespace nav_flags {
inline constexpr uint16_t kWalk = 1u << 0;
inline constexpr uint16_t kCrouch = 1u << 1;
inline constexpr uint16_t kDrop = 1u << 2; // one-way off-mesh link
inline constexpr uint16_t kAll = kWalk | kCrouch | kDrop;
} // namespace nav_flags

enum class NavPartition : uint8_t { Watershed = 0, Monotone = 1, Layers = 2 };

struct NavParams {
	float cell_size = 0.25f; // plan §3.6
	float cell_height = 0.1f; // climb / ch must be whole (topics/06 §2.4: ch 0.3 breaks Metro's stairs)
	float agent_radius = 0.35f; // MotorParams::radius
	float agent_height = 1.8f; // stand PoseParams::height
	float crouch_height = 1.15f; // crouch PoseParams::height: the walkable height; below agent_height = crouch area
	float agent_climb = 0.3f; // stand PoseParams::step_height
	float max_slope_degrees = 50.0f; // MotorParams::max_ascend_degrees
	// Walkable radius in cells: ceil(radius / cs) (Recast's convention: 2 cells = 0.5 m at cs 0.25) when true,
	// else round to nearest (1 cell = 0.25 m). docs §5.3 says which the gates use and why.
	bool radius_round_up = true;
	float max_edge_length = 12.0f; // metres (Recast sample: 12)
	float max_simplification_error = 1.3f; // cells
	uint32_t min_region_cells = 8 * 8;
	uint32_t merge_region_cells = 20 * 20;
	float detail_sample_distance = 6.0f; // cells x cs
	float detail_sample_max_error = 1.0f; // cells x ch
	NavPartition partition = NavPartition::Watershed;
	// Drop-down links (docs §5.3): from a border edge, a landing up to drop_max lower within drop_reach outward.
	// 4.9 m is IW4L's 192 in; a 4.9 m fall lands at 9.8 m/s, below the 13.9 m/s damage knee (fall-damage curve).
	bool drop_links = true;
	float drop_max = 4.9f;
	float drop_reach = 1.0f;
	float drop_sample_spacing = 1.0f;
	float region_margin = 32.0f; // combat-area margin of the baked tiles (gap load §2.7: area + 32 m)

	bool operator==(const NavParams &) const = default;
};

// The soldier's numbers from the motor params (radius, stand and crouch heights, stand step height, max ascend).
NavParams nav_params_from_motor(const MotorParams &p_motor);
// Canonical key text of every field (floats as bit patterns), the "params" of the nav cache keys.
std::string nav_params_text(const NavParams &p_params);
// "recast=1.6.0;polyref64=1;verts_per_poly=6": what changes a baked tile's bytes beyond our own code.
std::string recast_build_id();
inline constexpr uint32_t kNavFormatVersion = 1;
inline constexpr uint32_t kNavConverterVersion = 1;
inline constexpr const char *kNavTileCacheKind = "nav-tile";
inline constexpr const char *kNavWorldCacheKind = "nav-world";
inline constexpr const char *kNavAreaCacheKind = "nav-area";

// Bounds (docs §9): per tile at most kMaxNavTileInputTriangles input triangles, a rasterisation of at most
// kMaxNavTileRasterCells cells summed over its triangles' bounding boxes (charged before rasterising: a hostile soup
// of huge overlapping triangles fails the tile instead of allocating spans without end), a height range of at most
// kMaxNavTileHeight metres (Recast's 13-bit span height at ch 0.1 is 819 m); at most kMaxNavTiles tiles per world.
inline constexpr size_t kMaxNavTileInputTriangles = size_t(1) << 21;
inline constexpr uint64_t kMaxNavTileRasterCells = uint64_t(1) << 28;
inline constexpr float kMaxNavTileHeight = 800.0f;
inline constexpr size_t kMaxNavTiles = size_t(1) << 16;

using NavTileKey = std::array<int32_t, 2>; // (x, z) on the soup's 64 m grid

// One tile's input: the movement-layer triangles of the tile and of its neighbours that reach into the tile's
// border, in soup order (tile order, then triangle order), as Recast takes them (float xyz, int indices).
struct NavTileInput {
	NavTileKey tile{};
	std::vector<float> vertices; // xyz per corner, 3 corners per triangle, unshared
	std::vector<uint8_t> layers; // StaticLayer per triangle
	size_t own_triangles = 0; // triangles of this soup tile (the first own_triangles)
	core::Sha256Digest digest{}; // over every input triangle's float bits and layer, in order: the "nav-tile" key
};

// The input of p_tiles (sorted, unique) from p_soup with the layers in p_layer_mask (query_mask::kMovement for the
// bake; kAll only for the split investigation, docs §5.1). Fails past kMaxNavTileInputTriangles.
core::Result<std::vector<NavTileInput>> nav_tile_inputs(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles,
		const NavParams &p_params, uint32_t p_layer_mask = query_mask::kMovement);

struct NavTileInfo {
	NavTileKey tile{};
	uint32_t input_triangles = 0;
	uint32_t polys = 0;
	uint32_t vertices = 0;
	uint32_t detail_triangles = 0;
	uint32_t drop_links = 0;
	uint32_t data_bytes = 0; // the Detour tile data (dtCreateNavMeshData output)
	core::Sha256Digest input{}; // NavTileInput::digest
	double seconds = 0.0; // bake of this tile (0 when restored)
};

struct NavStats {
	size_t tiles = 0; // with at least one poly
	size_t tiles_empty = 0; // requested, no walkable poly
	size_t polys = 0;
	size_t drop_links = 0;
	uint64_t data_bytes = 0; // sum of the tiles' data (the navmesh's resident size, minus Detour's per-tile header)
	size_t tiles_hit = 0;
	size_t tiles_baked = 0;
	bool world_hit = false;
	double seconds_input = 0.0; // nav_tile_inputs
	double seconds_bake = 0.0; // Recast + link pass + dtCreateNavMeshData, all lanes, wall
	double seconds_assemble = 0.0; // dtNavMesh::init + addTile, serial
	double seconds_restore = 0.0;
	double seconds_total = 0.0;
	uint64_t peak_working_set = 0; // bytes, sampled after the bake (0 when not sampled)
};

// The immutable navmesh of one level x mode. Shared as std::shared_ptr<const NavWorld> on any thread.
class NavWorld {
public:
	~NavWorld();
	NavWorld(const NavWorld &) = delete;
	NavWorld &operator=(const NavWorld &) = delete;

	const NavParams &params() const;
	size_t tile_count() const;
	const NavTileInfo &tile(size_t p_index) const; // sorted by (x, z)
	const NavStats &stats() const;
	size_t poly_count() const; // over every tile, off-mesh links included
	Vec3 min() const;
	Vec3 max() const;

	struct Impl; // bf3_sim internal (src/nav_internal.h): the dtNavMesh, the global poly index
	const Impl &impl() const { return *impl_; }

private:
	friend struct NavWorldBuilder; // nav.cpp
	explicit NavWorld(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

struct NavBakeOptions {
	NavParams params;
	unsigned threads = 0; // parallel_for lanes over the tiles; 0 = all cores
	uint32_t layer_mask = query_mask::kMovement;
	// Validates each drop link with a stand-capsule sweep (kMovement) along its path; null = keep every link found.
	const CollisionWorld *link_check = nullptr;
	std::atomic<size_t> *tiles_done = nullptr; // progress (optional)
};

// Bakes p_tiles from p_soup (no cache).
core::Result<std::shared_ptr<const NavWorld>> bake_nav_world(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles,
		const NavBakeOptions &p_options = {});
// The same from loose triangles tiled like the soup (synthetic worlds for the tests).
core::Result<std::shared_ptr<const NavWorld>> bake_nav_world(std::span<const StaticTriangle> p_triangles, std::span<const NavTileKey> p_tiles,
		const NavBakeOptions &p_options = {});

struct NavLoadOptions {
	NavBakeOptions bake;
	StaticWorldLoadOptions world; // the soup options of the cold path (terrain_cover included); must equal the static world's
	std::vector<NavTileKey> region; // the tiles to bake (level_combat_tiles + margin)
};
// Through the cache (header comment). p_soup: the soup the caller already built (the static world's cold path), or
// null (then a nav-world miss builds it on p_world, calling thread). p_shapes: the static world (its link checks and
// source digest). p_cache may be null.
core::Result<std::shared_ptr<const NavWorld>> load_nav_world_cached(core::Cache *p_cache, const core::level::LoadedLevel &p_level,
		const core::level::World &p_world, const core::level::CollisionSoup *p_soup, const StaticWorldShapes &p_shapes, const NavLoadOptions &p_options);

// ---- blobs (exposed for the tests: round trips, hostile bytes, key changes) --------------------------------
// "OBF3NTIL" v1: NavTileInfo, then the Detour tile data. decode checks every count and offset of the Detour data
// against Detour 1.6.0's own layout (dtMeshHeader counts, the aligned sections of dtCreateNavMeshData) and every
// index inside it (poly vertices < vertCount, neis 0 / external-link flags / <= polyCount, detail meshes inside the
// detail arrays, BV nodes, off-mesh connection polys) before Detour sees it: dtNavMesh::addTile trusts the bytes.
std::vector<uint8_t> encode_nav_tile_blob(const NavWorld &p_world, size_t p_tile);
core::Status validate_detour_tile(std::span<const uint8_t> p_data, const NavParams &p_params, const NavTileKey &p_tile);
core::Result<std::shared_ptr<const NavWorld>> decode_nav_tile_blobs(std::span<const std::span<const uint8_t>> p_tiles, const NavParams &p_params);
core::CacheKeyInputs nav_tile_key_inputs(const core::Sha256Digest &p_input, const NavParams &p_params);
core::CacheKeyInputs nav_world_key_inputs(const core::Sha256Digest &p_source_digest, std::string_view p_level, std::string_view p_mode,
		const NavLoadOptions &p_options);

// ---- connectivity (docs §5.5) ---------------------------------------------------------------------------------
// Strongly connected components of the directed poly graph: every poly link of every tile (internal neighbours,
// portals between tiles) both ways, every off-mesh drop link one way. Iterative Tarjan, O(polys + links).
struct NavComponents {
	std::vector<uint32_t> component; // per global poly index (NavWorld::Impl's numbering)
	std::vector<uint32_t> polys; // per component: poly count
	std::vector<double> area; // per component: walkable area in m^2 (ground polys)
	uint32_t largest = 0; // by area
	size_t weak_count = 0; // weakly connected components (links both ways), for the report
};
NavComponents nav_components(const NavWorld &p_world);

// Where a gameplay point lies relative to the mesh: the nearest poly (any flag) inside a box of p_half_extents
// around p_point, its closest point (detail height), the horizontal and vertical offsets and the poly's component.
struct NavPointCheck {
	NavPolyRef poly = 0; // 0 = nothing inside the box
	Vec3 nearest{};
	float horizontal = 0.0f; // |xz(nearest - point)|
	float vertical = 0.0f; // nearest.y - point.y
	uint32_t component = UINT32_MAX;
};
NavPointCheck nav_check_point(const NavWorld &p_world, const NavComponents &p_components, const Vec3 &p_point, const Vec3 &p_half_extents);

// The mesh as triangles for the Godot debug overlay (detail triangles, coloured by component and area) and the drop
// links as segments; built on a loader thread.
struct NavDebugMesh {
	std::vector<Vec3> triangles; // 3 per triangle, world space, lifted 5 cm
	std::vector<uint32_t> triangle_component;
	std::vector<uint8_t> triangle_area;
	std::vector<Vec3> links; // 2 per drop link
};
NavDebugMesh nav_debug_mesh(const NavWorld &p_world, const NavComponents *p_components);

} // namespace bf3::sim
