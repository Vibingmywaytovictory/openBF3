// NavQuery and NavCorridor (nav_query.h; docs/design/s1.md §5.6): Detour's dtNavMeshQuery with a fixed node pool,
// the sliced A*, random points by area inside a component, and dtPathCorridor for the walkers. Area costs: walk 1,
// crouch 1.5 (crouching is slower), drop 1.
#include "nav_internal.h"

#include <DetourCommon.h>
#include <DetourPathCorridor.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bf3::sim {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point p_start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - p_start).count();
}

uint64_t splitmix64(uint64_t &r_state) {
	uint64_t z = (r_state += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

float unit(uint64_t &r_state) {
	return float(splitmix64(r_state) >> 40) / float(1ull << 24);
}

} // namespace

// ---- NavQuery -----------------------------------------------------------------------------------------------------

NavQuery::NavQuery(std::unique_ptr<Impl> p_impl) :
		impl_(std::move(p_impl)) {}

NavQuery::~NavQuery() = default;

core::Result<std::unique_ptr<NavQuery>> NavQuery::create(std::shared_ptr<const NavWorld> p_world, const NavQueryOptions &p_options) {
	if (p_world == nullptr || p_world->impl().mesh == nullptr) {
		return core::make_error("nav query: no navmesh");
	}
	if (p_options.max_nodes == 0 || p_options.max_nodes > 65535) {
		return core::make_error("nav query: max_nodes must be 1..65535");
	}
	auto impl = std::make_unique<Impl>();
	impl->world = std::move(p_world);
	impl->options = p_options;
	impl->query.reset(dtAllocNavMeshQuery());
	if (impl->query == nullptr || dtStatusFailed(impl->query->init(impl->world->impl().mesh.get(), int(p_options.max_nodes)))) {
		return core::make_error("nav query: dtNavMeshQuery::init failed");
	}
	impl->filter.setIncludeFlags(p_options.include_flags);
	impl->filter.setExcludeFlags(p_options.exclude_flags);
	impl->filter.setAreaCost(kNavAreaWalk, 1.0f);
	impl->filter.setAreaCost(kNavAreaCrouch, 1.5f);
	impl->filter.setAreaCost(kNavAreaDrop, 1.0f);
	return std::unique_ptr<NavQuery>(new NavQuery(std::move(impl)));
}

const NavWorld &NavQuery::world() const {
	return *impl_->world;
}

const NavQueryStats &NavQuery::stats() const {
	return impl_->stats;
}

bool NavQuery::nearest(const Vec3 &p_point, const Vec3 &p_half_extents, NavPolyRef *r_poly, Vec3 *r_point) {
	Impl &m = *impl_;
	const Clock::time_point start = Clock::now();
	++m.stats.nearest;
	dtPolyRef ref = 0;
	float p[3] = { 0, 0, 0 };
	const bool ok = dtStatusSucceed(m.query->findNearestPoly(p_point.data(), p_half_extents.data(), &m.filter, &ref, p)) && ref != 0;
	m.stats.ms_nearest += ms_since(start);
	if (!ok) {
		return false;
	}
	if (r_poly != nullptr) {
		*r_poly = NavPolyRef(ref);
	}
	if (r_point != nullptr) {
		*r_point = { p[0], p[1], p[2] };
	}
	return true;
}

bool NavQuery::random_point(uint64_t &r_rng, const NavComponents &p_components, uint32_t p_component, NavPolyRef *r_poly, Vec3 *r_point) {
	Impl &m = *impl_;
	++m.stats.random_points;
	const NavWorld::Impl &w = m.world->impl();
	Impl::AreaTable *table = nullptr;
	for (Impl::AreaTable &t : m.tables) {
		if (t.component == p_component && t.components == &p_components) {
			table = &t;
		}
	}
	if (table == nullptr) {
		Impl::AreaTable t;
		t.component = p_component;
		t.components = &p_components;
		double sum = 0.0;
		const dtNavMesh &mesh = *w.mesh;
		for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
			const dtMeshTile *tile = mesh.getTile(ti);
			if (tile == nullptr || tile->header == nullptr) {
				continue;
			}
			const dtPolyRef base = mesh.getPolyRefBase(tile);
			for (int pi = 0; pi < tile->header->polyCount; ++pi) {
				const dtPoly &poly = tile->polys[pi];
				if (poly.getType() != DT_POLYTYPE_GROUND || !m.filter.passFilter(base | dtPolyRef(pi), tile, &poly)) {
					continue;
				}
				const uint32_t g = w.poly_base[size_t(ti)] + uint32_t(pi);
				if (p_component != UINT32_MAX && (g >= p_components.component.size() || p_components.component[g] != p_component)) {
					continue;
				}
				double a = 0.0;
				for (int k = 2; k < poly.vertCount; ++k) {
					a += double(dtTriArea2D(&tile->verts[poly.verts[0] * 3], &tile->verts[poly.verts[k - 1] * 3], &tile->verts[poly.verts[k] * 3]));
				}
				a = std::fabs(a);
				if (a <= 0.0) {
					continue;
				}
				sum += a;
				t.cumulative.push_back(sum);
				t.polys.push_back(NavPolyRef(base | dtPolyRef(pi)));
			}
		}
		m.tables.push_back(std::move(t));
		table = &m.tables.back();
	}
	if (table->cumulative.empty()) {
		return false;
	}
	const double pick = double(unit(r_rng)) * table->cumulative.back();
	size_t at = size_t(std::upper_bound(table->cumulative.begin(), table->cumulative.end(), pick) - table->cumulative.begin());
	at = std::min(at, table->polys.size() - 1);
	const NavPolyRef ref = table->polys[at];
	const dtMeshTile *tile = nullptr;
	const dtPoly *poly = nullptr;
	if (dtStatusFailed(w.mesh->getTileAndPolyByRef(dtPolyRef(ref), &tile, &poly))) {
		return false;
	}
	float verts[3 * DT_VERTS_PER_POLYGON];
	float areas[DT_VERTS_PER_POLYGON];
	for (int k = 0; k < poly->vertCount; ++k) {
		dtVcopy(&verts[k * 3], &tile->verts[poly->verts[k] * 3]);
	}
	float pt[3];
	const float s = unit(r_rng);
	const float t = unit(r_rng);
	dtRandomPointInConvexPoly(verts, poly->vertCount, areas, s, t, pt);
	float h = pt[1];
	m.query->getPolyHeight(dtPolyRef(ref), pt, &h);
	pt[1] = h;
	if (r_poly != nullptr) {
		*r_poly = ref;
	}
	if (r_point != nullptr) {
		*r_point = { pt[0], pt[1], pt[2] };
	}
	return true;
}

NavPathStatus NavQuery::begin_path(NavPolyRef p_start, const Vec3 &p_start_point, NavPolyRef p_goal, const Vec3 &p_goal_point) {
	Impl &m = *impl_;
	const Clock::time_point start = Clock::now();
	++m.stats.paths_started;
	const dtStatus st = m.query->initSlicedFindPath(dtPolyRef(p_start), dtPolyRef(p_goal), p_start_point.data(), p_goal_point.data(), &m.filter, 0);
	m.stats.ms_path += ms_since(start);
	if (dtStatusFailed(st)) {
		m.sliced_active = false;
		++m.stats.paths_failed;
		return NavPathStatus::Failed;
	}
	m.sliced_active = true;
	return NavPathStatus::InProgress;
}

NavPathStatus NavQuery::continue_path(uint32_t p_max_iterations, uint32_t *r_iterations) {
	Impl &m = *impl_;
	if (r_iterations != nullptr) {
		*r_iterations = 0;
	}
	if (!m.sliced_active) {
		return NavPathStatus::Idle;
	}
	const Clock::time_point start = Clock::now();
	int done = 0;
	const dtStatus st = m.query->updateSlicedFindPath(int(std::max<uint32_t>(p_max_iterations, 1)), &done);
	m.stats.ms_path += ms_since(start);
	m.stats.path_iterations += uint64_t(std::max(done, 0));
	if (r_iterations != nullptr) {
		*r_iterations = uint32_t(std::max(done, 0));
	}
	if (dtStatusFailed(st)) {
		m.sliced_active = false;
		++m.stats.paths_failed;
		return NavPathStatus::Failed;
	}
	return dtStatusInProgress(st) ? NavPathStatus::InProgress : NavPathStatus::Done;
}

NavPathStatus NavQuery::finish_path(std::vector<NavPolyRef> &r_polys) {
	Impl &m = *impl_;
	r_polys.clear();
	if (!m.sliced_active) {
		return NavPathStatus::Idle;
	}
	const Clock::time_point start = Clock::now();
	dtPolyRef path[kMaxNavPathPolys];
	int n = 0;
	const dtStatus st = m.query->finalizeSlicedFindPath(path, &n, int(kMaxNavPathPolys));
	m.stats.ms_path += ms_since(start);
	m.sliced_active = false;
	if (dtStatusFailed(st) || n <= 0) {
		++m.stats.paths_failed;
		return NavPathStatus::Failed;
	}
	r_polys.assign(path, path + n);
	if (dtStatusDetail(st, DT_PARTIAL_RESULT) || dtStatusDetail(st, DT_OUT_OF_NODES)) {
		++m.stats.paths_partial;
		return NavPathStatus::Partial;
	}
	++m.stats.paths_done;
	return NavPathStatus::Done;
}

void NavQuery::cancel_path() {
	Impl &m = *impl_;
	if (m.sliced_active) {
		// Detour has no cancel: finalize into a scratch path and drop it.
		dtPolyRef path[4];
		int n = 0;
		m.query->finalizeSlicedFindPath(path, &n, 4);
		m.sliced_active = false;
	}
}

bool nav_closest_point(NavQuery &p_query, NavPolyRef p_poly, const Vec3 &p_point, Vec3 *r_point) {
	float out[3];
	bool over = false;
	if (dtStatusFailed(p_query.impl().query->closestPointOnPoly(dtPolyRef(p_poly), p_point.data(), out, &over))) {
		return false;
	}
	*r_point = { out[0], out[1], out[2] };
	return true;
}

// ---- NavCorridor ----------------------------------------------------------------------------------------------------

struct NavCorridor::Impl {
	dtPathCorridor corridor;
	bool initialised = false;
	bool empty = true;
};

NavCorridor::NavCorridor() :
		impl_(std::make_unique<Impl>()) {}

NavCorridor::~NavCorridor() = default;
NavCorridor::NavCorridor(NavCorridor &&) noexcept = default;
NavCorridor &NavCorridor::operator=(NavCorridor &&) noexcept = default;

void NavCorridor::reset(std::span<const NavPolyRef> p_polys, const Vec3 &p_position, const Vec3 &p_goal) {
	Impl &m = *impl_;
	if (!m.initialised) {
		m.initialised = m.corridor.init(int(kMaxNavPathPolys));
	}
	if (!m.initialised || p_polys.empty()) {
		m.empty = true;
		return;
	}
	const size_t n = std::min<size_t>(p_polys.size(), kMaxNavPathPolys);
	m.corridor.reset(dtPolyRef(p_polys[0]), p_position.data());
	std::vector<dtPolyRef> path(p_polys.begin(), p_polys.begin() + ptrdiff_t(n));
	m.corridor.setCorridor(p_goal.data(), path.data(), int(n));
	m.empty = false;
}

void NavCorridor::clear() {
	impl_->empty = true;
}

bool NavCorridor::empty() const {
	return impl_->empty;
}

bool NavCorridor::move_position(NavQuery &p_query, const Vec3 &p_position) {
	Impl &m = *impl_;
	if (m.empty) {
		return false;
	}
	NavQuery::Impl &q = p_query.impl();
	const Clock::time_point start = Clock::now();
	++q.stats.corridor_moves;
	const bool ok = m.corridor.movePosition(p_position.data(), q.query.get(), &q.filter);
	q.stats.ms_corridor += ms_since(start);
	if (!ok) {
		return false;
	}
	// moveAlongSurface clamps the corridor's position at the mesh edge: an agent far from it left the corridor.
	const float *p = m.corridor.getPos();
	const float dx = p[0] - p_position[0], dz = p[2] - p_position[2];
	return dx * dx + dz * dz <= 1.0f && std::fabs(p[1] - p_position[1]) <= 2.5f;
}

uint32_t NavCorridor::corners(NavQuery &p_query, NavCorner *r_corners, uint32_t p_max) {
	Impl &m = *impl_;
	if (m.empty || r_corners == nullptr) {
		return 0;
	}
	const uint32_t max = std::min<uint32_t>(p_max, kMaxNavCorners);
	NavQuery::Impl &q = p_query.impl();
	float verts[kMaxNavCorners * 3];
	unsigned char flags[kMaxNavCorners];
	dtPolyRef polys[kMaxNavCorners];
	const Clock::time_point start = Clock::now();
	const int n = m.corridor.findCorners(verts, flags, polys, int(max), q.query.get(), &q.filter);
	q.stats.ms_corridor += ms_since(start);
	const NavWorld &world = *q.world;
	for (int i = 0; i < n; ++i) {
		NavCorner &c = r_corners[i];
		c = NavCorner();
		c.position = { verts[i * 3], verts[i * 3 + 1], verts[i * 3 + 2] };
		c.poly = NavPolyRef(polys[i]);
		nav_poly_area_flags(world, c.poly, &c.area, &c.flags);
		c.end = (flags[i] & DT_STRAIGHTPATH_END) != 0;
		if ((flags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) != 0) {
			c.flags = uint16_t(c.flags | nav_flags::kDrop);
		}
	}
	return uint32_t(std::max(n, 0));
}

bool NavCorridor::valid(NavQuery &p_query, uint32_t p_ahead) const {
	Impl &m = *impl_;
	if (m.empty) {
		return false;
	}
	NavQuery::Impl &q = p_query.impl();
	return m.corridor.isValid(int(p_ahead), q.query.get(), &q.filter);
}

bool NavCorridor::move_over_drop(NavQuery &p_query, const NavCorner &p_corner, Vec3 *r_end) {
	Impl &m = *impl_;
	if (m.empty || (p_corner.flags & nav_flags::kDrop) == 0) {
		return false;
	}
	NavQuery::Impl &q = p_query.impl();
	dtPolyRef refs[2] = { 0, 0 };
	float start[3], end[3];
	if (!m.corridor.moveOverOffmeshConnection(dtPolyRef(p_corner.poly), refs, start, end, q.query.get())) {
		return false;
	}
	if (r_end != nullptr) {
		*r_end = { end[0], end[1], end[2] };
	}
	return true;
}

Vec3 NavCorridor::position() const {
	const float *p = impl_->corridor.getPos();
	return { p[0], p[1], p[2] };
}

Vec3 NavCorridor::goal() const {
	const float *p = impl_->corridor.getTarget();
	return { p[0], p[1], p[2] };
}

NavPolyRef NavCorridor::first_poly() const {
	return impl_->empty ? 0 : NavPolyRef(impl_->corridor.getFirstPoly());
}

} // namespace bf3::sim
