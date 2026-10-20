#pragma once

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
	std::vector<NavTileInfo> tiles;
	std::vector<std::vector<uint8_t>> data;
	NavStats stats;
	std::unique_ptr<dtNavMesh, NavMeshDeleter> mesh;
	std::vector<uint32_t> poly_base;
	std::vector<uint32_t> tile_polys;
	size_t polys = 0;
	Vec3 min{};
	Vec3 max{};

	uint32_t global(NavPolyRef p_ref) const;

	NavPolyRef ref_of(uint32_t p_global) const;
};

struct NavWorldBuilder {

	static core::Result<std::shared_ptr<const NavWorld>> finish(const NavParams &p_params, std::vector<NavTileInfo> p_tiles,
			std::vector<std::vector<uint8_t>> p_data, NavStats p_stats);
};

struct NavCells {
	int walkable_height = 0;
	int stand_height = 0;
	int walkable_climb = 0;
	int walkable_radius = 0;
	int border = 0;
	int tile_cells = 0;
	int max_edge = 0;
};
NavCells nav_cells(const NavParams &p_params);

core::Status check_nav_params(const NavParams &p_params);

class NavLinkWorldPool {
public:
	explicit NavLinkWorldPool(std::shared_ptr<const StaticWorldShapes> p_shapes);
	~NavLinkWorldPool();

	core::Result<CollisionWorld *> checkout(bool *r_created, double *r_seconds);
	void give_back(CollisionWorld *p_world);
	size_t created() const;

private:
	std::shared_ptr<const StaticWorldShapes> shapes_;
	mutable std::mutex mutex_;
	std::vector<std::unique_ptr<CollisionWorld>> all_;
	std::vector<CollisionWorld *> free_;
};

struct NavTileBake {
	NavTileInfo info;
	std::vector<uint8_t> data;
	size_t link_candidates = 0;
	size_t link_sweeps = 0;
	size_t links_rejected = 0;
	double seconds_links = 0.0;
};
core::Result<NavTileBake> bake_nav_tile(const NavTileInput &p_input, const NavParams &p_params, const CollisionWorld *p_links, rcContext &r_context);

core::Result<std::vector<NavTileInput>> nav_tile_inputs_threads(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles,
		const NavParams &p_params, uint32_t p_layer_mask, unsigned p_threads, std::span<const core::level::WaterPlane> p_water);

core::Result<std::vector<NavTileInput>> nav_tile_inputs_triangles(std::span<const StaticTriangle> p_triangles, std::span<const NavTileKey> p_tiles,
		const NavParams &p_params, uint32_t p_layer_mask, unsigned p_threads);

core::Result<std::shared_ptr<const NavWorld>> bake_nav_inputs(std::vector<NavTileInput> p_inputs, const NavBakeOptions &p_options, NavStats p_stats,
		core::Cache *p_cache = nullptr, bool *r_stored = nullptr);

std::vector<uint8_t> encode_nav_tile(const NavTileInfo &p_info, std::span<const uint8_t> p_data);
core::Status decode_nav_tile(std::span<const uint8_t> p_blob, const NavParams &p_params, NavTileInfo &r_info, std::vector<uint8_t> &r_data);

struct NavQuery::Impl {
	std::shared_ptr<const NavWorld> world;
	std::unique_ptr<dtNavMeshQuery, NavMeshQueryDeleter> query;
	dtQueryFilter filter;
	NavQueryOptions options;
	NavQueryStats stats;
	bool sliced_active = false;

	struct AreaTable {
		uint32_t component = UINT32_MAX;
		const NavComponents *components = nullptr;
		std::vector<double> cumulative;
		std::vector<NavPolyRef> polys;
	};
	std::vector<AreaTable> tables;
};

bool nav_closest_point(NavQuery &p_query, NavPolyRef p_poly, const Vec3 &p_point, Vec3 *r_point);

void nav_poly_area_flags(const NavWorld &p_world, NavPolyRef p_poly, uint8_t *r_area, uint16_t *r_flags);

bool nav_offmesh_end(const NavWorld &p_world, NavPolyRef p_poly, Vec3 *r_start, Vec3 *r_end);

}
