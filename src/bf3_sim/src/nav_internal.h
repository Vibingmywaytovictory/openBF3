#pragma once
// bf3_sim internal (not a public header; Recast and Detour stay PRIVATE to bf3_sim): NavWorld::Impl, NavQuery::Impl,
// the per-tile bake and the assembly shared by the bake (nav_bake.cpp), the cache (nav_cache.cpp), the components
// (nav_components.cpp), the queries (nav_query.cpp) and the walker brain (bots/), spike S1 (docs/design/s1.md §5).
//
// Poly numbering: a NavWorld adds its tiles to the dtNavMesh in (x, z) order, so Detour hands out tile indices
// 0, 1, 2, ... (dtNavMesh::init chains the free list from index 0); the global poly index of a ref is
// poly_base[decodePolyIdTile(ref)] + decodePolyIdPoly(ref) (off-mesh connection polys included, they follow a
// tile's ground polys). Detour tile data: dtNavMesh::addTile writes the link lists into the bytes it is given, so
// the NavWorld keeps the pristine bytes (what the cache stores and validate_detour_tile checks) and hands Detour
// its own copy (DT_TILE_FREE_DATA).

#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <memory>
#include <mutex>
#include <span>
#include <vector>

class rcContext;

namespace bf3::sim {

class CollisionWorld;

static_assert(sizeof(NavPolyRef) == sizeof(dtPolyRef), "build Detour with RECASTNAVIGATION_DT_POLYREF64 (docs/design/s1.md §5.3)");

struct NavMeshDeleter {
	void operator()(dtNavMesh *p_mesh) const { dtFreeNavMesh(p_mesh); }
};
struct NavMeshQueryDeleter {
	void operator()(dtNavMeshQuery *p_query) const { dtFreeNavMeshQuery(p_query); }
};

struct NavWorld::Impl {
	NavParams params;
	std::vector<NavTileInfo> tiles; // every requested tile, sorted by (x, z); empty ones have polys 0 and no data
	std::vector<std::vector<uint8_t>> data; // per tile: the pristine Detour tile data (empty for an empty tile)
	NavStats stats;
	std::unique_ptr<dtNavMesh, NavMeshDeleter> mesh;
	std::vector<uint32_t> poly_base; // per Detour tile index (0 .. maxTiles-1): first global poly index
	std::vector<uint32_t> tile_polys; // per Detour tile index: polys (ground + off-mesh)
	size_t polys = 0;
	Vec3 min{};
	Vec3 max{};

	// The global poly index of a ref, UINT32_MAX when the ref names no poly.
	uint32_t global(NavPolyRef p_ref) const;
	// The ref of a global poly index (0 when out of range).
	NavPolyRef ref_of(uint32_t p_global) const;
};

// Builds NavWorlds from finished tiles (the friend of NavWorld).
struct NavWorldBuilder {
	// p_tiles / p_data in (x, z) order (strictly increasing). Inits the dtNavMesh (64 m tiles, origin 0) and adds
	// every non-empty tile in that order (a copy of its bytes), fills the poly numbering, bounds and the stats'
	// totals; seconds_assemble is measured here. Fails when Detour refuses a tile (the error names it).
	static core::Result<std::shared_ptr<const NavWorld>> finish(const NavParams &p_params, std::vector<NavTileInfo> p_tiles,
			std::vector<std::vector<uint8_t>> p_data, NavStats p_stats);
};

// Recast's integer parameters of a NavParams (docs §5.3).
struct NavCells {
	int walkable_height = 0; // crouch height, ceil(crouch / ch)
	int stand_height = 0; // ceil(stand / ch): spans with less room are crouch area
	int walkable_climb = 0; // lround(climb / ch); check_nav_params requires it whole
	int walkable_radius = 0; // ceil(radius / cs) or round to nearest
	int border = 0; // walkable_radius + 3
	int tile_cells = 0; // 64 / cs
	int max_edge = 0; // max_edge_length / cs
};
NavCells nav_cells(const NavParams &p_params);
// Checks the params a bake needs (finite, positive, cs dividing 64 m, the drop numbers); the error names the field.
core::Status check_nav_params(const NavParams &p_params);

// A pool of link-check CollisionWorlds over one StaticWorldShapes, at most one per lane (docs §3): checkout creates
// a world when the free list is empty. Thread-safe.
class NavLinkWorldPool {
public:
	explicit NavLinkWorldPool(std::shared_ptr<const StaticWorldShapes> p_shapes);
	~NavLinkWorldPool();
	// A world for this tile job (null with the error when CollisionWorld::create fails); r_created says whether
	// this call created it and r_seconds how long that took.
	core::Result<CollisionWorld *> checkout(bool *r_created, double *r_seconds);
	void give_back(CollisionWorld *p_world);
	size_t created() const;

private:
	std::shared_ptr<const StaticWorldShapes> shapes_;
	mutable std::mutex mutex_;
	std::vector<std::unique_ptr<CollisionWorld>> all_;
	std::vector<CollisionWorld *> free_;
};

// One tile's bake (Recast + drop links + dtCreateNavMeshData) from its input. p_links: the lane's link-check world
// (null = keep every drop link found; synthetic tests). r_data is empty when the tile has no walkable poly.
struct NavTileBake {
	NavTileInfo info;
	std::vector<uint8_t> data;
	size_t link_candidates = 0;
	size_t link_sweeps = 0;
	size_t links_rejected = 0;
	double seconds_links = 0.0;
};
core::Result<NavTileBake> bake_nav_tile(const NavTileInput &p_input, const NavParams &p_params, const CollisionWorld *p_links, rcContext &r_context);

// nav_tile_inputs on up to p_threads lanes (the digests, and TC's terrain cells and water rule, per tile on the lanes).
core::Result<std::vector<NavTileInput>> nav_tile_inputs_threads(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles,
		const NavParams &p_params, uint32_t p_layer_mask, unsigned p_threads, std::span<const core::level::WaterPlane> p_water);
// The same over loose triangles tiled by the soup's rule (synthetic worlds).
core::Result<std::vector<NavTileInput>> nav_tile_inputs_triangles(std::span<const StaticTriangle> p_triangles, std::span<const NavTileKey> p_tiles,
		const NavParams &p_params, uint32_t p_layer_mask, unsigned p_threads);

// Bakes prepared inputs on the lanes and assembles (bake_nav_world's body and nav_cache.cpp's cold path). With
// p_cache (open), each tile goes through its "nav-tile" entry: a hit decodes and validates it, a miss bakes (checking
// a link world out only then) and puts; r_stored (optional) says whether every tile is in the cache afterwards.
// p_stats carries the caller's seconds_input; the bake's counters and times are added.
core::Result<std::shared_ptr<const NavWorld>> bake_nav_inputs(std::vector<NavTileInput> p_inputs, const NavBakeOptions &p_options, NavStats p_stats,
		core::Cache *p_cache = nullptr, bool *r_stored = nullptr);

// "OBF3NTIL" v1 of one tile (layout in nav_cache.cpp) and its decoder: every count against the bytes left, trailing
// bytes refused, the Detour data through validate_detour_tile.
std::vector<uint8_t> encode_nav_tile(const NavTileInfo &p_info, std::span<const uint8_t> p_data);
core::Status decode_nav_tile(std::span<const uint8_t> p_blob, const NavParams &p_params, NavTileInfo &r_info, std::vector<uint8_t> &r_data);

struct NavQuery::Impl {
	std::shared_ptr<const NavWorld> world;
	std::unique_ptr<dtNavMeshQuery, NavMeshQueryDeleter> query;
	dtQueryFilter filter;
	NavQueryOptions options;
	NavQueryStats stats;
	bool sliced_active = false;
	// random_point: per component, the cumulative ground-poly areas (built on first use).
	struct AreaTable {
		uint32_t component = UINT32_MAX;
		const NavComponents *components = nullptr;
		std::vector<double> cumulative;
		std::vector<NavPolyRef> polys;
	};
	std::vector<AreaTable> tables;
};

// The closest point of a poly to p_point (Detour closestPointOnPoly), false for a bad ref.
bool nav_closest_point(NavQuery &p_query, NavPolyRef p_poly, const Vec3 &p_point, Vec3 *r_point);
// Area and flags of a poly (0, 0 for a bad ref).
void nav_poly_area_flags(const NavWorld &p_world, NavPolyRef p_poly, uint8_t *r_area, uint16_t *r_flags);
// The end point of an off-mesh connection poly (its landing), false when p_poly is not one.
bool nav_offmesh_end(const NavWorld &p_world, NavPolyRef p_poly, Vec3 *r_start, Vec3 *r_end);

} // namespace bf3::sim
