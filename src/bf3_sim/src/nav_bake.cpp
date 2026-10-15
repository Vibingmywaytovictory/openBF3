// The per-tile Recast bake (docs/design/s1.md §5.3), the drop-down links with their sweep checks on per-lane
// CollisionWorlds (§3), and bake_nav_world (tile inputs -> lanes -> assembly). The pipeline is Recast's
// Sample_TileMesh with three S1 rules:
//   * walkable triangles are judged two-sided (|normal.y| >= cos slope): the motor collides with back faces (m1.md
//     §5.1), so a floor whose triangles face down is walkable for both;
//   * the walkable height is the crouch height; spans with less than the stand height of room become
//     kNavAreaCrouch (after rcBuildCompactHeightfield, before the erosion), so crouch-only passages stay in the mesh;
//   * before rasterising, the cells each triangle's box covers inside the heightfield are summed and charged against
//     kMaxNavTileRasterCells (hostile input fails the tile instead of allocating without end).
#include "nav_internal.h"

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/thread_pool.h"
#include "bf3/sim/collision_world.h"

#include <DetourAlloc.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>

namespace bf3::sim {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point p_start) {
	return std::chrono::duration<double>(Clock::now() - p_start).count();
}

struct HeightfieldDeleter {
	void operator()(rcHeightfield *p) const { rcFreeHeightField(p); }
};
struct CompactDeleter {
	void operator()(rcCompactHeightfield *p) const { rcFreeCompactHeightfield(p); }
};
struct ContourDeleter {
	void operator()(rcContourSet *p) const { rcFreeContourSet(p); }
};
struct PolyMeshDeleter {
	void operator()(rcPolyMesh *p) const { rcFreePolyMesh(p); }
};
struct DetailDeleter {
	void operator()(rcPolyMeshDetail *p) const { rcFreePolyMeshDetail(p); }
};

constexpr int kMaxOffMeshPerTile = 1 << 14;
constexpr float kSweepLift = 0.05f; // feet above the surface for the link sweeps

std::string tile_name(const NavTileKey &p_tile) {
	return "nav tile (" + std::to_string(p_tile[0]) + ", " + std::to_string(p_tile[1]) + ")";
}

struct DropLink {
	float start[3];
	float end[3];
};

// The ground of one heightfield column below p_max_y (the highest span top at most p_max_y): found, its height, and
// whether it was walkable before the erosion. inside = the column lies in the compact heightfield.
struct Column {
	bool inside = false;
	bool found = false;
	bool walkable = false;
	float top = 0.0f;
};
Column ground_below(const rcCompactHeightfield &p_chf, const std::vector<uint8_t> &p_areas, float p_x, float p_z, float p_max_y) {
	Column out;
	const int cx = int(std::floor((p_x - p_chf.bmin[0]) / p_chf.cs));
	const int cz = int(std::floor((p_z - p_chf.bmin[2]) / p_chf.cs));
	if (cx < 0 || cz < 0 || cx >= p_chf.width || cz >= p_chf.height) {
		return out;
	}
	out.inside = true;
	const rcCompactCell &cell = p_chf.cells[size_t(cx) + size_t(cz) * size_t(p_chf.width)];
	for (unsigned int s = cell.index; s < cell.index + cell.count; ++s) {
		const float top = p_chf.bmin[1] + float(p_chf.spans[s].y) * p_chf.ch;
		if (top <= p_max_y && (!out.found || top > out.top)) {
			out.found = true;
			out.top = top;
			out.walkable = p_areas[s] != RC_NULL_AREA;
		}
	}
	return out;
}

// The drop-down links of one tile (docs §5.3 "Drop links"): every border edge of the poly mesh without a neighbour
// (not a tile portal), sampled every drop_sample_spacing; outward from each sample, the ledge (the first column whose
// ground lies more than a climb lower, cell by cell) and the landing a capsule radius and a cell beyond it, within
// drop_reach, found in the compact heightfield's columns (areas as they were before the erosion), more than the climb
// and at most drop_max lower; with p_world, a stand-capsule sweep out to the landing and one down to it must both be
// clear.
void find_drop_links(const rcPolyMesh &p_mesh, const rcCompactHeightfield &p_chf, const std::vector<uint8_t> &p_areas, const NavParams &p_params,
		const NavCells &p_cells, const CollisionWorld *p_world, NavTileBake &r_bake, std::vector<DropLink> &r_links) {
	const int nvp = p_mesh.nvp;
	const float cs = p_mesh.cs;
	const float ch = p_mesh.ch;
	const float *bmin = p_mesh.bmin;
	const float climb = float(p_cells.walkable_climb) * ch;
	for (int i = 0; i < p_mesh.npolys; ++i) {
		const unsigned short *p = &p_mesh.polys[size_t(i) * 2 * size_t(nvp)];
		int nv = 0;
		float cx = 0.0f, cz = 0.0f;
		for (int j = 0; j < nvp && p[j] != RC_MESH_NULL_IDX; ++j) {
			const unsigned short *v = &p_mesh.verts[size_t(p[j]) * 3];
			cx += bmin[0] + float(v[0]) * cs;
			cz += bmin[2] + float(v[2]) * cs;
			++nv;
		}
		if (nv < 3) {
			continue;
		}
		cx /= float(nv);
		cz /= float(nv);
		for (int j = 0; j < nv; ++j) {
			const unsigned short nei = p[nvp + j];
			if (nei != RC_MESH_NULL_IDX) {
				continue; // a neighbour, or a portal to the next tile (0x8000 | side)
			}
			const unsigned short *va = &p_mesh.verts[size_t(p[j]) * 3];
			const unsigned short *vb = &p_mesh.verts[size_t(p[(j + 1) % nv]) * 3];
			const float ax = bmin[0] + float(va[0]) * cs, ay = bmin[1] + float(va[1]) * ch, az = bmin[2] + float(va[2]) * cs;
			const float bx = bmin[0] + float(vb[0]) * cs, by = bmin[1] + float(vb[1]) * ch, bz = bmin[2] + float(vb[2]) * cs;
			const float ex = bx - ax, ez = bz - az;
			const float len = std::sqrt(ex * ex + ez * ez);
			if (len < 1e-4f) {
				continue;
			}
			float nx = ez / len, nz = -ex / len;
			const float mx = 0.5f * (ax + bx), mz = 0.5f * (az + bz);
			if (nx * (mx - cx) + nz * (mz - cz) < 0.0f) {
				nx = -nx;
				nz = -nz;
			}
			const int samples = std::max(1, int(len / p_params.drop_sample_spacing));
			for (int k = 0; k < samples; ++k) {
				if (int(r_links.size()) >= kMaxOffMeshPerTile) {
					return;
				}
				const float t = (float(k) + 0.5f) / float(samples);
				const float sx = ax + ex * t, sy = ay + (by - ay) * t, sz = az + ez * t;
				// Walk outward one cell at a time to the ledge: the first column whose ground (the highest surface at
				// most a climb above the start) is more than a climb below the start, or has none. The poly edge lies
				// the erosion (walkable radius) plus up to the contour error inside the real ledge, so a fixed reach
				// would put the landing capsule over the ledge's lip (found by sim.nav.unit's 3 m ledge).
				float ledge = -1.0f;
				for (float d = 0.5f * cs; d <= p_params.drop_reach; d += cs) {
					const Column c = ground_below(p_chf, p_areas, sx + nx * d, sz + nz * d, sy + climb);
					if (!c.inside) {
						break;
					}
					if (!c.found || sy - c.top > climb + 0.5f * ch) {
						ledge = d;
						break;
					}
				}
				if (ledge < 0.0f) {
					continue;
				}
				// The landing: a capsule radius and a cell beyond the ledge, so the fall clears the lip.
				const float reach = ledge + p_params.agent_radius + cs;
				if (reach > p_params.drop_reach) {
					continue;
				}
				const float px = sx + nx * reach, pz = sz + nz * reach;
				const Column land = ground_below(p_chf, p_areas, px, pz, sy + climb);
				if (!land.inside || !land.found || !land.walkable) {
					continue;
				}
				const float drop = sy - land.top;
				if (drop <= climb + 0.5f * ch || drop > p_params.drop_max) {
					continue;
				}
				++r_bake.link_candidates;
				if (p_world != nullptr) {
					const Vec3 feet{ sx, sy + kSweepLift, sz };
					const Vec3 out{ nx * reach, 0.0f, nz * reach };
					++r_bake.link_sweeps;
					if (p_world->sweep_capsule(feet, p_params.agent_radius, p_params.agent_height, out, query_mask::kMovement)) {
						++r_bake.links_rejected;
						continue;
					}
					const Vec3 over{ px, sy + kSweepLift, pz };
					const Vec3 down{ 0.0f, -drop, 0.0f };
					++r_bake.link_sweeps;
					if (p_world->sweep_capsule(over, p_params.agent_radius, p_params.agent_height, down, query_mask::kMovement)) {
						++r_bake.links_rejected;
						continue;
					}
				}
				DropLink link;
				link.start[0] = sx;
				link.start[1] = sy;
				link.start[2] = sz;
				link.end[0] = px;
				link.end[1] = land.top;
				link.end[2] = pz;
				r_links.push_back(link);
			}
		}
	}
}

} // namespace

core::Result<NavTileBake> bake_nav_tile(const NavTileInput &p_input, const NavParams &p_params, const CollisionWorld *p_links, rcContext &r_context) {
	const Clock::time_point start = Clock::now();
	const std::string where = tile_name(p_input.tile);
	NavTileBake out;
	out.info.tile = p_input.tile;
	out.info.input = p_input.digest;
	const size_t ntris = p_input.layers.size();
	out.info.input_triangles = uint32_t(ntris);
	if (p_input.vertices.size() != ntris * 9) {
		return core::make_error(where + ": input vertices and layers differ");
	}
	if (ntris == 0) {
		out.info.seconds = seconds_since(start);
		return out;
	}
	const NavCells c = nav_cells(p_params);
	const float cs = p_params.cell_size;
	const float ch = p_params.cell_height;
	const float border = float(c.border) * cs;
	float bmin[3] = { float(p_input.tile[0]) * kNavTileSize - border, 0.0f, float(p_input.tile[1]) * kNavTileSize - border };
	float bmax[3] = { float(p_input.tile[0] + 1) * kNavTileSize + border, 0.0f, float(p_input.tile[1] + 1) * kNavTileSize + border };
	// The y range of the triangles that reach into the heightfield's columns, and the raster charge (docs §9).
	float ymin = 1e30f, ymax = -1e30f;
	uint64_t cells = 0;
	const float *v = p_input.vertices.data();
	for (size_t t = 0; t < ntris; ++t) {
		const float *a = v + t * 9;
		const float x0 = std::max(std::min({ a[0], a[3], a[6] }), bmin[0]), x1 = std::min(std::max({ a[0], a[3], a[6] }), bmax[0]);
		const float z0 = std::max(std::min({ a[2], a[5], a[8] }), bmin[2]), z1 = std::min(std::max({ a[2], a[5], a[8] }), bmax[2]);
		if (x0 > x1 || z0 > z1) {
			continue;
		}
		cells += uint64_t((x1 - x0) / cs + 2.0f) * uint64_t((z1 - z0) / cs + 2.0f);
		if (cells > kMaxNavTileRasterCells) {
			return core::make_error(where + ": the triangles cover more than " + std::to_string(kMaxNavTileRasterCells) + " raster cells");
		}
		ymin = std::min({ ymin, a[1], a[4], a[7] });
		ymax = std::max({ ymax, a[1], a[4], a[7] });
	}
	if (ymin > ymax) {
		out.info.seconds = seconds_since(start);
		return out; // nothing reaches the heightfield
	}
	bmin[1] = ymin - ch;
	bmax[1] = std::min(ymax + ch, bmin[1] + kMaxNavTileHeight);

	const int size = c.tile_cells + 2 * c.border;
	std::unique_ptr<rcHeightfield, HeightfieldDeleter> hf(rcAllocHeightfield());
	if (hf == nullptr || !rcCreateHeightfield(&r_context, *hf, size, size, bmin, bmax, cs, ch)) {
		return core::make_error(where + ": rcCreateHeightfield failed");
	}
	// Two-sided walkable rule (header comment).
	std::vector<unsigned char> areas(ntris, RC_NULL_AREA);
	const float cos_slope = std::cos(p_params.max_slope_degrees * (3.14159265358979323846f / 180.0f));
	for (size_t t = 0; t < ntris; ++t) {
		const float *a = v + t * 9;
		const float e0[3] = { a[3] - a[0], a[4] - a[1], a[5] - a[2] };
		const float e1[3] = { a[6] - a[0], a[7] - a[1], a[8] - a[2] };
		const float n[3] = { e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2], e0[0] * e1[1] - e0[1] * e1[0] };
		const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (len > 0.0f && std::fabs(n[1]) / len > cos_slope) {
			areas[t] = RC_WALKABLE_AREA;
		}
	}
	if (!rcRasterizeTriangles(&r_context, v, areas.data(), int(ntris), *hf, c.walkable_climb)) {
		return core::make_error(where + ": rcRasterizeTriangles failed");
	}
	areas = std::vector<unsigned char>();
	rcFilterLowHangingWalkableObstacles(&r_context, c.walkable_climb, *hf);
	rcFilterLedgeSpans(&r_context, c.walkable_height, c.walkable_climb, *hf);
	rcFilterWalkableLowHeightSpans(&r_context, c.walkable_height, *hf);

	std::unique_ptr<rcCompactHeightfield, CompactDeleter> chf(rcAllocCompactHeightfield());
	if (chf == nullptr || !rcBuildCompactHeightfield(&r_context, c.walkable_height, c.walkable_climb, *hf, *chf)) {
		return core::make_error(where + ": rcBuildCompactHeightfield failed");
	}
	hf.reset();
	// Walk and crouch areas by the room above each span.
	for (int s = 0; s < chf->spanCount; ++s) {
		if (chf->areas[s] == RC_NULL_AREA) {
			continue;
		}
		chf->areas[s] = int(chf->spans[s].h) < c.stand_height ? kNavAreaCrouch : kNavAreaWalk;
	}
	std::vector<uint8_t> areas_before(chf->areas, chf->areas + chf->spanCount); // the drop landings' rule
	if (!rcErodeWalkableArea(&r_context, c.walkable_radius, *chf)) {
		return core::make_error(where + ": rcErodeWalkableArea failed");
	}
	const int min_region = int(p_params.min_region_cells);
	const int merge_region = int(p_params.merge_region_cells);
	switch (p_params.partition) {
		case NavPartition::Watershed:
			if (!rcBuildDistanceField(&r_context, *chf) || !rcBuildRegions(&r_context, *chf, c.border, min_region, merge_region)) {
				return core::make_error(where + ": watershed regions failed");
			}
			break;
		case NavPartition::Monotone:
			if (!rcBuildRegionsMonotone(&r_context, *chf, c.border, min_region, merge_region)) {
				return core::make_error(where + ": monotone regions failed");
			}
			break;
		case NavPartition::Layers:
			if (!rcBuildLayerRegions(&r_context, *chf, c.border, min_region)) {
				return core::make_error(where + ": layer regions failed");
			}
			break;
	}
	std::unique_ptr<rcContourSet, ContourDeleter> cset(rcAllocContourSet());
	if (cset == nullptr || !rcBuildContours(&r_context, *chf, p_params.max_simplification_error, c.max_edge, *cset)) {
		return core::make_error(where + ": rcBuildContours failed");
	}
	if (cset->nconts == 0) {
		out.info.seconds = seconds_since(start);
		return out;
	}
	std::unique_ptr<rcPolyMesh, PolyMeshDeleter> pmesh(rcAllocPolyMesh());
	if (pmesh == nullptr || !rcBuildPolyMesh(&r_context, *cset, DT_VERTS_PER_POLYGON, *pmesh)) {
		return core::make_error(where + ": rcBuildPolyMesh failed");
	}
	cset.reset();
	if (pmesh->npolys == 0) {
		out.info.seconds = seconds_since(start);
		return out;
	}
	if (pmesh->nverts >= 0xffff) {
		return core::make_error(where + ": " + std::to_string(pmesh->nverts) + " poly mesh vertices exceed Detour's 16-bit indices");
	}
	std::unique_ptr<rcPolyMeshDetail, DetailDeleter> dmesh(rcAllocPolyMeshDetail());
	const float sample_dist = p_params.detail_sample_distance < 0.9f ? 0.0f : cs * p_params.detail_sample_distance;
	if (dmesh == nullptr || !rcBuildPolyMeshDetail(&r_context, *pmesh, *chf, sample_dist, ch * p_params.detail_sample_max_error, *dmesh)) {
		return core::make_error(where + ": rcBuildPolyMeshDetail failed");
	}
	for (int i = 0; i < pmesh->npolys; ++i) {
		pmesh->flags[i] = pmesh->areas[i] == kNavAreaCrouch ? nav_flags::kCrouch : nav_flags::kWalk;
	}

	std::vector<DropLink> links;
	if (p_params.drop_links) {
		const Clock::time_point link_start = Clock::now();
		find_drop_links(*pmesh, *chf, areas_before, p_params, c, p_links, out, links);
		out.seconds_links = seconds_since(link_start);
	}
	chf.reset();
	std::vector<float> con_verts(links.size() * 6);
	std::vector<float> con_rad(links.size(), std::max(p_params.agent_radius, 2.0f * cs));
	std::vector<unsigned char> con_dir(links.size(), 0); // one way
	std::vector<unsigned char> con_area(links.size(), kNavAreaDrop);
	std::vector<unsigned short> con_flags(links.size(), nav_flags::kDrop);
	std::vector<unsigned int> con_id(links.size());
	for (size_t i = 0; i < links.size(); ++i) {
		std::memcpy(&con_verts[i * 6], links[i].start, sizeof(links[i].start));
		std::memcpy(&con_verts[i * 6 + 3], links[i].end, sizeof(links[i].end));
		con_id[i] = unsigned(i);
	}

	dtNavMeshCreateParams params{};
	params.verts = pmesh->verts;
	params.vertCount = pmesh->nverts;
	params.polys = pmesh->polys;
	params.polyAreas = pmesh->areas;
	params.polyFlags = pmesh->flags;
	params.polyCount = pmesh->npolys;
	params.nvp = pmesh->nvp;
	params.detailMeshes = dmesh->meshes;
	params.detailVerts = dmesh->verts;
	params.detailVertsCount = dmesh->nverts;
	params.detailTris = dmesh->tris;
	params.detailTriCount = dmesh->ntris;
	params.offMeshConVerts = links.empty() ? nullptr : con_verts.data();
	params.offMeshConRad = links.empty() ? nullptr : con_rad.data();
	params.offMeshConDir = links.empty() ? nullptr : con_dir.data();
	params.offMeshConAreas = links.empty() ? nullptr : con_area.data();
	params.offMeshConFlags = links.empty() ? nullptr : con_flags.data();
	params.offMeshConUserID = links.empty() ? nullptr : con_id.data();
	params.offMeshConCount = int(links.size());
	params.walkableHeight = p_params.crouch_height;
	params.walkableRadius = p_params.agent_radius;
	params.walkableClimb = p_params.agent_climb;
	params.tileX = p_input.tile[0];
	params.tileY = p_input.tile[1];
	params.tileLayer = 0;
	std::memcpy(params.bmin, pmesh->bmin, sizeof(params.bmin));
	std::memcpy(params.bmax, pmesh->bmax, sizeof(params.bmax));
	params.cs = cs;
	params.ch = ch;
	params.buildBvTree = true;
	unsigned char *data = nullptr;
	int data_size = 0;
	if (!dtCreateNavMeshData(&params, &data, &data_size)) {
		return core::make_error(where + ": dtCreateNavMeshData failed (" + std::to_string(pmesh->npolys) + " polys)");
	}
	out.data.assign(data, data + data_size);
	dtFree(data);
	out.info.polys = uint32_t(pmesh->npolys);
	out.info.vertices = uint32_t(pmesh->nverts);
	out.info.detail_triangles = uint32_t(dmesh->ntris);
	out.info.drop_links = uint32_t(links.size());
	out.info.data_bytes = uint32_t(out.data.size());
	out.info.seconds = seconds_since(start);
	return out;
}

// ---- the link-check world pool ----------------------------------------------------------------------------------

NavLinkWorldPool::NavLinkWorldPool(std::shared_ptr<const StaticWorldShapes> p_shapes) :
		shapes_(std::move(p_shapes)) {}

NavLinkWorldPool::~NavLinkWorldPool() = default;

core::Result<CollisionWorld *> NavLinkWorldPool::checkout(bool *r_created, double *r_seconds) {
	if (r_created != nullptr) {
		*r_created = false;
	}
	if (r_seconds != nullptr) {
		*r_seconds = 0.0;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!free_.empty()) {
			CollisionWorld *w = free_.back();
			free_.pop_back();
			return w;
		}
	}
	// Created outside the lock: another lane may create its own meanwhile (at most one per lane in flight).
	const Clock::time_point start = Clock::now();
	core::Result<std::unique_ptr<CollisionWorld>> made = CollisionWorld::create(shapes_);
	if (!made) {
		return made.error();
	}
	CollisionWorld *w = made->get();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		all_.push_back(std::move(*made));
	}
	if (r_created != nullptr) {
		*r_created = true;
	}
	if (r_seconds != nullptr) {
		*r_seconds = seconds_since(start);
	}
	return w;
}

void NavLinkWorldPool::give_back(CollisionWorld *p_world) {
	if (p_world == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	free_.push_back(p_world);
}

size_t NavLinkWorldPool::created() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return all_.size();
}

// ---- bake ---------------------------------------------------------------------------------------------------------

core::Result<std::shared_ptr<const NavWorld>> bake_nav_inputs(std::vector<NavTileInput> p_inputs, const NavBakeOptions &p_options, NavStats p_stats,
		core::Cache *p_cache, bool *r_stored) {
	BF3_TRY(check_nav_params(p_options.params));
	core::Cache *cache = (p_cache != nullptr && p_cache->is_open()) ? p_cache : nullptr;
	const bool link_checked = p_options.link_shapes != nullptr && p_options.params.drop_links;
	std::optional<NavLinkWorldPool> pool;
	if (link_checked) {
		pool.emplace(p_options.link_shapes);
	}
	std::vector<std::optional<core::Result<NavTileBake>>> baked(p_inputs.size());
	std::vector<double> setup(p_inputs.size(), 0.0);
	std::vector<core::CachedItem> items(p_inputs.size());
	const Clock::time_point start = Clock::now();
	core::parallel_for(
			p_inputs.size(),
			[&](size_t p_i) {
				const NavTileInput &input = p_inputs[p_i];
				const auto bake = [&]() -> core::Result<NavTileBake> {
					CollisionWorld *world = nullptr;
					if (pool) {
						bool created = false;
						core::Result<CollisionWorld *> w = pool->checkout(&created, &setup[p_i]);
						if (!w) {
							return w.error();
						}
						world = *w;
					}
					rcContext context(false);
					core::Result<NavTileBake> r = bake_nav_tile(input, p_options.params, world, context);
					if (pool) {
						pool->give_back(world);
					}
					return r;
				};
				if (cache == nullptr) {
					baked[p_i].emplace(bake());
				} else {
					const std::optional<core::CacheKey> key = core::make_cache_key(nav_tile_key_inputs(input.digest, p_options.params, link_checked));
					if (!key) {
						baked[p_i].emplace(core::make_error("nav tile: no cache key"));
						return;
					}
					std::optional<NavTileBake> got;
					const auto accept = [&](std::span<const uint8_t> p_blob) -> core::Status {
						NavTileBake b;
						BF3_TRY(decode_nav_tile(p_blob, p_options.params, b.info, b.data));
						if (b.info.tile != input.tile || b.info.input != input.digest) {
							return core::make_error("nav-tile entry describes another tile");
						}
						got = std::move(b);
						return core::Status::success();
					};
					const auto produce = [&]() -> core::Result<std::vector<uint8_t>> {
						core::Result<NavTileBake> r = bake();
						if (!r) {
							return r.error();
						}
						got = std::move(*r);
						return encode_nav_tile(got->info, got->data);
					};
					core::Result<core::CachedItem> item = core::cached_convert(cache, *key, accept, produce);
					if (!item) {
						baked[p_i].emplace(item.error());
						return;
					}
					items[p_i] = *item;
					if (!got) {
						baked[p_i].emplace(core::make_error("nav tile: the cache gave no tile"));
						return;
					}
					baked[p_i].emplace(std::move(*got));
				}
				// The input is no longer needed (its digest went into the tile info).
				p_inputs[p_i].vertices = std::vector<float>();
				p_inputs[p_i].layers = std::vector<uint8_t>();
				if (p_options.tiles_done != nullptr) {
					p_options.tiles_done->fetch_add(1, std::memory_order_relaxed);
				}
			},
			p_options.threads);
	p_stats.seconds_bake = seconds_since(start);
	std::vector<NavTileInfo> infos;
	std::vector<std::vector<uint8_t>> data;
	infos.reserve(baked.size());
	data.reserve(baked.size());
	bool stored = cache != nullptr;
	for (size_t i = 0; i < baked.size(); ++i) {
		if (!baked[i]->ok()) {
			return baked[i]->error();
		}
		NavTileBake &b = baked[i]->value();
		p_stats.link_candidates += b.link_candidates;
		p_stats.link_sweeps += b.link_sweeps;
		p_stats.links_rejected += b.links_rejected;
		p_stats.seconds_link_cpu += b.seconds_links;
		p_stats.seconds_link_setup += setup[i];
		if (cache != nullptr && items[i].outcome == core::CacheOutcome::Hit) {
			++p_stats.tiles_hit;
		} else {
			++p_stats.tiles_baked;
		}
		stored = stored && items[i].outcome != core::CacheOutcome::Uncached;
		infos.push_back(b.info);
		data.push_back(std::move(b.data));
	}
	if (r_stored != nullptr) {
		*r_stored = stored;
	}
	p_stats.link_worlds = pool ? pool->created() : 0;
	return NavWorldBuilder::finish(p_options.params, std::move(infos), std::move(data), p_stats);
}

core::Result<std::shared_ptr<const NavWorld>> bake_nav_world(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles,
		const NavBakeOptions &p_options) {
	const Clock::time_point start = Clock::now();
	core::Result<std::vector<NavTileInput>> inputs = nav_tile_inputs_threads(p_soup, p_tiles, p_options.params, p_options.layer_mask, p_options.threads);
	if (!inputs) {
		return inputs.error();
	}
	NavStats stats;
	stats.seconds_input = seconds_since(start);
	for (const NavTileInput &in : *inputs) {
		stats.far_triangles += in.far_triangles;
	}
	return bake_nav_inputs(std::move(*inputs), p_options, stats);
}

core::Result<std::shared_ptr<const NavWorld>> bake_nav_world(std::span<const StaticTriangle> p_triangles, std::span<const NavTileKey> p_tiles,
		const NavBakeOptions &p_options) {
	const Clock::time_point start = Clock::now();
	core::Result<std::vector<NavTileInput>> inputs = nav_tile_inputs_triangles(p_triangles, p_tiles, p_options.params, p_options.layer_mask, p_options.threads);
	if (!inputs) {
		return inputs.error();
	}
	NavStats stats;
	stats.seconds_input = seconds_since(start);
	for (const NavTileInput &in : *inputs) {
		stats.far_triangles += in.far_triangles;
	}
	return bake_nav_inputs(std::move(*inputs), p_options, stats);
}

} // namespace bf3::sim
