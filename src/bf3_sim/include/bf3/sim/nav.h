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
// (the union of the level's MP modes' combat-area tiles + margin, mode_areas.h). A tile's input is every
// movement-layer triangle of the soup, from ANY soup tile, whose XZ bounding box overlaps the tile's input box (the
// tile grown by nav_input_border): the soup files a triangle under its centroid's tile and never clips it, and a
// Jolt tile MeshShape keeps the whole triangle, so the motor collides with a long triangle (Metro: up to 205 m, 112
// Collision triangles reach past the 3 x 3 neighbourhood of their tile; Kharg 154 [verified 2026-10-15]) wherever it
// lies; a fixed 3 x 3 gather would leave holes there that the motor does not have (docs §5.3). Tiles are therefore
// independent and bake on parallel_for lanes; walkable height is the crouch height and spans with less than the
// stand height of room are area kNavAreaCrouch. Drop-down off-mesh links (one way) are found per tile from its own
// input and checked with a stand-capsule sweep on a per-lane CollisionWorld (NavBakeOptions::link_shapes); the
// input box reaches past every point such a sweep can touch, so the tile's digest covers the link check too.
// Detour tiles are assembled into one dtNavMesh (64-bit poly refs,
// RECASTNAVIGATION_DT_POLYREF64; Kharg has 1,494 combat-area tiles in CQL alone and dense tiles may need more than
// the 1,024 polys a 32-bit ref leaves at 12 tile bits).
//
// Cache (bf3_core Cache, docs §5.4): "nav-tile" (one Detour tile's data + its NavTileInfo; params = the SHA-256 of
// the tile's input triangles (everything it rasterises or sweeps against), nav_params_text, recast_build_id, and
// whether drop links were sweep-checked on Jolt, with the Jolt build id), "nav-world" (the tile list of a
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
#include <functional>
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
	// Plan §3.6 says 0.25. Implementation (docs §5.7 as built): 0.2, so that Recast's whole-cell erosion of 2 cells is
	// 0.4 m, just over the 0.35 m capsule. At 0.25 the radius is 0.5 m (ceil: closes the 0.7-1.0 m passages the
	// motor walks through; Metro CQL kept 13 of 78 G2 rows in the main component) or 0.25 m (nearest: 68 rows, but
	// the corridors then pass gaps the capsule does not, and half of the walkers' goals were abandoned in the G1 run).
	float cell_size = 0.2f;
	float cell_height = 0.1f; // climb / ch must be whole (topics/06 §2.4: ch 0.3 breaks Metro's stairs)
	float agent_radius = 0.35f; // MotorParams::radius
	float agent_height = 1.8f; // stand PoseParams::height
	float crouch_height = 1.15f; // crouch PoseParams::height: the walkable height; below agent_height = crouch area
	float agent_climb = 0.3f; // stand PoseParams::step_height
	float max_slope_degrees = 50.0f; // MotorParams::max_ascend_degrees
	// Walkable radius in cells: ceil(radius / cs) (Recast's convention: 2 cells = 0.4 m at cs 0.2, 0.5 m at 0.25) when
	// true, else round to nearest (2 cells at 0.2, 1 at 0.25). docs §5.3 / §5.7 say which the gates use and why. The
	// conservative raster grows every wall by up to one more cell, so the mesh keeps 0.4-0.6 m off walls at the default.
	bool radius_round_up = true;
	float max_edge_length = 12.0f; // metres (Recast sample: 12)
	float max_simplification_error = 1.3f; // cells
	uint32_t min_region_cells = 8 * 8;
	uint32_t merge_region_cells = 20 * 20;
	float detail_sample_distance = 6.0f; // cells x cs
	// Implementation (docs §5.7 as built): 2 ch = 0.2 m, not the Recast sample's 1 ch. rcBuildPolyMeshDetail was 76 %
	// of Caspian's bake CPU at 1 ch (65 of 85 s summed over the lanes; G4 6.2 s at cs 0.2) and is 27 s at 2 ch (G4
	// 3.9 s). The detail mesh only gives the poly heights the queries return; the motor stands on the physics world.
	float detail_sample_max_error = 2.0f; // cells x ch
	NavPartition partition = NavPartition::Watershed;
	// Drop-down links (docs §5.3): from a border edge, a landing up to drop_max lower within drop_reach outward.
	// 4.9 m is IW4L's 192 in; a 4.9 m fall lands at 9.8 m/s, below the 13.9 m/s damage knee (fall-damage curve).
	// drop_reach is the farthest the landing may lie from the poly edge: the edge sits the erosion (0.4-0.5 m) plus up
	// to the contour error (1.3 cells) inside the real ledge, and the landing must be a capsule radius and a cell past
	// the ledge, so 1.5 m (implementation: revision 3's fixed 1.0 m put the falling capsule on the lip, sim.nav.unit).
	bool drop_links = true;
	float drop_max = 4.9f;
	float drop_reach = 1.5f;
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
// kMaxNavTileRasterCells cells summed over its triangles' bounding boxes clipped to the tile's heightfield (Recast
// clips each triangle to the heightfield, so that is what it touches; charged before rasterising: a hostile soup of
// huge overlapping triangles fails the tile instead of allocating spans without end), a height range of at most
// kMaxNavTileHeight metres (Recast's 13-bit span height at ch 0.1 is 819 m); at most kMaxNavTiles tiles per world;
// the input gather files each triangle under every requested tile its box overlaps, at most kMaxNavInputRefs
// (triangle, tile) pairs over the whole bake (each pair charged; a triangle's tile range is clamped to the requested
// tiles' range before it is walked, so a triangle spanning the region costs its tiles, never more).
inline constexpr size_t kMaxNavTileInputTriangles = size_t(1) << 21;
inline constexpr uint64_t kMaxNavTileRasterCells = uint64_t(1) << 28;
inline constexpr uint64_t kMaxNavInputRefs = uint64_t(1) << 26;
inline constexpr float kMaxNavTileHeight = 800.0f;
inline constexpr size_t kMaxNavTiles = size_t(1) << 16;

using NavTileKey = std::array<int32_t, 2>; // (x, z) on the soup's 64 m grid

// How far past its 64 m square a tile's input box reaches (metres): the larger of Recast's border (walkable radius
// in cells + 3 cells, as Sample_TileMesh; 0.4 + 0.6 = 1.0 m at the defaults) and, when drop_links is on, the reach
// of a drop link's check sweep (drop_reach + agent_radius + one cell = 1.5 + 0.35 + 0.2 = 2.05 m). So the input
// digest covers every triangle the tile's raster and its link sweeps can touch (review of revision 1: the sweep
// reached 1.35 m past a 1.25 m border). The Recast heightfield border itself stays walkable radius + 3 cells.
float nav_input_border(const NavParams &p_params);
// Recast's border in cells (walkable radius cells + 3).
int nav_recast_border_cells(const NavParams &p_params);

// One tile's input, as Recast takes it (float xyz, int indices): every triangle of the soup whose layer
// (static_layer_of) is in the mask and whose XZ bounding box overlaps the tile's input box
// [64x - b, 64x + 64 + b) x [64z - b, 64z + 64 + b), b = nav_input_border, whatever soup tile files it; in soup
// order (soup tile (x, z) order, then triangle order), so the order and the digest are deterministic.
struct NavTileInput {
	NavTileKey tile{};
	std::vector<float> vertices; // xyz per corner, 3 corners per triangle, unshared
	std::vector<uint8_t> layers; // StaticLayer per triangle
	size_t own_triangles = 0; // of those, filed under this tile by the soup (a count; they are not stored first)
	size_t far_triangles = 0; // filed under a soup tile outside the 3 x 3 around this one (long triangles)
	// The "nav-tile" key: SHA-256 over the tile key (x, z) first, then every input triangle's float bits and layer,
	// in order. The tile key is needed and must stay: one large slab can be the whole input of two tiles, and the
	// baked Detour data carries the tile coordinates (validate_detour_tile checks them against the key's tile), so
	// two tiles with identical triangles must not share an entry (review of revision 2).
	core::Sha256Digest digest{};
};

// The input of p_tiles (sorted, unique) from p_soup with the layers in p_layer_mask (query_mask::kMovement for the
// bake; kAll only for the split investigation, docs §5.7). Gather: one pass over the soup's triangles, each filed
// under every requested tile its box overlaps (its tile range clamped to the requested tiles' range first, every
// pair charged against kMaxNavInputRefs), so the work is linear in the soup plus the pairs. Fails past
// kMaxNavTileInputTriangles per tile or kMaxNavInputRefs pairs.
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
	size_t tiles_uncached = 0; // baked with an open cache but not stored (then no world index is written)
	std::string cache_error; // the first such tile's put / get error
	size_t far_triangles = 0; // input triangles from outside a tile's 3 x 3 neighbourhood, summed over the tiles
	size_t link_worlds = 0; // per-lane CollisionWorlds created for the link checks
	size_t link_candidates = 0; // drop samples that found a landing
	size_t link_sweeps = 0;
	size_t links_rejected = 0; // the sweep hit something
	double seconds_input = 0.0; // nav_tile_inputs
	double seconds_bake = 0.0; // the lanes' wall time: Recast + link pass (world checkouts and sweeps) + dtCreateNavMeshData
	double seconds_link_setup = 0.0; // of the lanes' work, the per-lane CollisionWorld creations (summed over lanes)
	double seconds_link_cpu = 0.0; // of the lanes' work, the link pass incl. sweeps (summed over lanes)
	double seconds_assemble = 0.0; // dtNavMesh::init + addTile, serial
	double seconds_restore = 0.0;
	double seconds_total = 0.0;
	uint64_t peak_working_set = 0; // bytes, sampled after the bake (0 when not sampled)
};
// G4's timed span (docs §1): from the soup in memory to the assembled dtNavMesh, with the link checks (the
// per-lane world creations and the sweeps run inside the lanes, so inside seconds_bake).
inline double nav_bake_gate_seconds(const NavStats &p_stats) {
	return p_stats.seconds_input + p_stats.seconds_bake + p_stats.seconds_assemble;
}

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
	// Validates each drop link with a stand-capsule sweep (kMovement) along its path against the motor's static world;
	// null = keep every link found (synthetic tests only; the gates and the game pass the S1 static world).
	// Threads: a CollisionWorld runs on its owner's thread only (collision_world.h: one temp allocator and query
	// scratch per world), so the bake never shares one. It keeps a pool of CollisionWorld::create(link_shapes)
	// worlds, at most one per lane: a tile job checks a world (and its rcContext) out of a mutex-guarded free list,
	// creating one when the list is empty, and returns it when the tile is done (core::parallel_for hands out item
	// indices, not lane indices, hence the checkout). Two worlds over the same shapes may be queried at once
	// (collision_world.h). The worlds and their query counters die with the bake.
	std::shared_ptr<const StaticWorldShapes> link_shapes;
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
// p_link_checked: the tile's drop links were sweep-checked on Jolt ("links=jolt;" in the params and jolt_version =
// jolt_build_id()); an unchecked tile keys "links=none;", so the two never share an entry.
core::CacheKeyInputs nav_tile_key_inputs(const core::Sha256Digest &p_input, const NavParams &p_params, bool p_link_checked);
core::CacheKeyInputs nav_world_key_inputs(const core::Sha256Digest &p_source_digest, std::string_view p_level, std::string_view p_mode,
		const NavLoadOptions &p_options);

// ---- input = motor set, against the cooked world (docs §5.1 proof 1) --------------------------------------------
// Reads the triangles the motor really collides with back out of the cooked Jolt MeshShapes (Shape::
// GetTrianglesStart / GetTrianglesNext over the tile's input box, on every static tile whose StaticTileInfo bounds
// overlap it, on the layers of p_layer_mask; a returned triangle is kept only when its own XZ bounds overlap the
// box, since Jolt returns whole leaf blocks) and matches them one to one against p_input's triangles within
// p_tolerance per corner (Jolt stores vertices quantised to 21 bits over each shape's bounds, about 0.1 mm on a
// 200 m box, so the match is by tolerance with corner rotation allowed, not by bits). Independent of
// nav_tile_inputs: it starts from the cooked shapes, not from the soup. bf3_sim internal access to the shapes.
struct NavInputCheck {
	size_t nav_triangles = 0;
	size_t world_triangles = 0;
	size_t matched = 0;
	size_t nav_only = 0; // in the nav input, not in the motor's world: the nav walks where the motor has nothing
	size_t nav_only_degenerate = 0; // of those, triangles Jolt's MeshShapeSettings sanitise drops (no area; allowed)
	size_t world_only = 0; // in the motor's world, not in the nav input: a hole or a missing wall in the nav
	float max_corner_error = 0.0f; // metres, over the matched pairs
};
core::Result<NavInputCheck> nav_check_input_against_world(const StaticWorldShapes &p_world, const NavTileInput &p_input, const NavParams &p_params,
		uint32_t p_layer_mask = query_mask::kMovement, float p_tolerance = 0.001f);

// ---- connectivity (docs §5.5) ---------------------------------------------------------------------------------
// Strongly connected components of the directed poly graph: every poly link of every tile (internal neighbours,
// portals between tiles) both ways, every off-mesh drop link one way. Iterative Tarjan, O(polys + links).
struct NavComponents {
	std::vector<uint32_t> component; // per global poly index (NavWorld::Impl's numbering)
	std::vector<uint32_t> polys; // per component: poly count
	std::vector<double> area; // per component: walkable area in m^2 (ground polys)
	uint32_t largest = 0; // by area
	size_t weak_count = 0; // weakly connected components (links both ways), for the report
	// Per global poly: its weakly connected component (every link taken both ways), ids 0..weak_count-1 in poly order.
	// A G2 row outside the main SCC but in the main SCC's weak component is "one-way only" (it can be left by a drop
	// but not reached, or the reverse); one in another weak component is truly disconnected (review of revision 2:
	// the two call for different R6 responses, docs §5.5).
	std::vector<uint32_t> weak_component;
};
NavComponents nav_components(const NavWorld &p_world);
// Per component: 1 when it can reach p_target along the links (p_target itself included). A one-way G2 row that
// reaches the main component can be left towards it (a spawn there still feeds the fight); one that does not is a
// trap (implementation: the G2 report splits the one-way class by this). O(polys + links).
std::vector<uint8_t> nav_components_reaching(const NavWorld &p_world, const NavComponents &p_components, uint32_t p_target);
// Per component: the area (m^2) of its ground polys whose centroid (x, z) p_inside accepts. load_bot_level takes
// the mode's main component as the one with the most area inside the mode's combat areas (implementation: the
// largest over the whole region is another mode's playground on Metro, where TDM is played on the street level).
std::vector<double> nav_component_area_in(const NavWorld &p_world, const NavComponents &p_components, const std::function<bool(float, float)> &p_inside);
// The strongly connected component of a poly (UINT32_MAX for 0, a stale ref or a poly outside p_components). The
// walker brain uses it to keep goals inside its own component (docs §7 "Goals").
uint32_t nav_poly_component(const NavWorld &p_world, const NavComponents &p_components, NavPolyRef p_poly);

// Where a gameplay point lies relative to the mesh: the nearest poly (any flag) inside a box of p_half_extents
// around p_point, its closest point (detail height), the horizontal and vertical offsets and the poly's component.
struct NavPointCheck {
	NavPolyRef poly = 0; // 0 = nothing inside the box
	Vec3 nearest{};
	float horizontal = 0.0f; // |xz(nearest - point)|
	float vertical = 0.0f; // nearest.y - point.y
	uint32_t component = UINT32_MAX;
	uint32_t weak_component = UINT32_MAX; // NavComponents::weak_component of the poly
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
