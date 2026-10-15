// Suite sim-nav-unit (no data; spike S1, docs/design/s1.md §5, §9, §10): the navmesh on synthetic worlds. A floor
// is one component across tile seams; a 0.9 m door closes with Recast's ceil radius and opens with round-to-nearest;
// a 1.5 m ceiling gives crouch area; a 0.29 m step joins and a 0.45 m one does not; a 3 m ledge gets drop links and
// the components are one way; a 200 m slab filed under one tile walks in every tile it covers and the input equals
// the cooked world's triangles; the border rule; the link-check pool on 16 lanes; Raycast never enters; determinism;
// blob round trips, truncations and 2,000 mutated tile blobs; hostile inputs; cache keys; cold / warm on a scratch
// root; nav_components against a brute-force SCC; NavQuery and NavCorridor.
#include <doctest/doctest.h>

#include "nav_internal.h"

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/static_world.h"

#include <DetourNavMesh.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <cstring>
#include <set>
#include <unordered_map>
#include <vector>

using namespace bf3;
using namespace bf3::sim;

namespace {

namespace fs = std::filesystem;

void add_quad(std::vector<StaticTriangle> &r_out, const Vec3 &p_a, const Vec3 &p_b, const Vec3 &p_c, const Vec3 &p_d, StaticLayer p_layer) {
	StaticTriangle t;
	t.layer = p_layer;
	t.v = { p_a, p_b, p_c };
	r_out.push_back(t);
	t.v = { p_a, p_c, p_d };
	r_out.push_back(t);
}

// A floor at p_y over [x0, x1] x [z0, z1] (front face up).
void add_floor(std::vector<StaticTriangle> &r_out, float p_x0, float p_z0, float p_x1, float p_z1, float p_y, StaticLayer p_layer = StaticLayer::Collision) {
	add_quad(r_out, { p_x0, p_y, p_z0 }, { p_x0, p_y, p_z1 }, { p_x1, p_y, p_z1 }, { p_x1, p_y, p_z0 }, p_layer);
}

// A closed box (6 faces, outward).
void add_box(std::vector<StaticTriangle> &r_out, const Vec3 &p_min, const Vec3 &p_max, StaticLayer p_layer = StaticLayer::Collision) {
	const float x0 = p_min[0], y0 = p_min[1], z0 = p_min[2], x1 = p_max[0], y1 = p_max[1], z1 = p_max[2];
	add_quad(r_out, { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 }, p_layer); // top
	add_quad(r_out, { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 }, p_layer); // bottom
	add_quad(r_out, { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 }, p_layer); // -z
	add_quad(r_out, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 }, p_layer); // +z
	add_quad(r_out, { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 }, p_layer); // -x
	add_quad(r_out, { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 }, p_layer); // +x
}

// A block standing on the ground: top and four sides, no bottom (a closed box would rasterise its inside as a
// floor with room above it: Recast sees surfaces, not volumes).
void add_block(std::vector<StaticTriangle> &r_out, const Vec3 &p_min, const Vec3 &p_max, StaticLayer p_layer = StaticLayer::Collision) {
	const float x0 = p_min[0], y0 = p_min[1], z0 = p_min[2], x1 = p_max[0], y1 = p_max[1], z1 = p_max[2];
	add_quad(r_out, { x0, y1, z0 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y1, z0 }, p_layer); // top
	add_quad(r_out, { x0, y0, z0 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y0, z0 }, p_layer); // -z
	add_quad(r_out, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y1, z1 }, { x0, y1, z1 }, p_layer); // +z
	add_quad(r_out, { x0, y0, z0 }, { x0, y0, z1 }, { x0, y1, z1 }, { x0, y1, z0 }, p_layer); // -x
	add_quad(r_out, { x1, y0, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x1, y0, z1 }, p_layer); // +x
}

// Ground at p_y over [x0, x1] x [z0, z1] with rectangular holes (where blocks stand), as grid quads.
struct Rect {
	float x0, z0, x1, z1;
};
void add_ground(std::vector<StaticTriangle> &r_out, float p_x0, float p_z0, float p_x1, float p_z1, float p_y, const std::vector<Rect> &p_holes) {
	std::vector<float> xs{ p_x0, p_x1 }, zs{ p_z0, p_z1 };
	for (const Rect &h : p_holes) {
		xs.push_back(h.x0);
		xs.push_back(h.x1);
		zs.push_back(h.z0);
		zs.push_back(h.z1);
	}
	std::sort(xs.begin(), xs.end());
	std::sort(zs.begin(), zs.end());
	for (size_t i = 0; i + 1 < xs.size(); ++i) {
		for (size_t j = 0; j + 1 < zs.size(); ++j) {
			const float cx = 0.5f * (xs[i] + xs[i + 1]), cz = 0.5f * (zs[j] + zs[j + 1]);
			if (xs[i + 1] - xs[i] <= 0.0f || zs[j + 1] - zs[j] <= 0.0f) {
				continue;
			}
			bool inside = false;
			for (const Rect &h : p_holes) {
				inside = inside || (cx > h.x0 && cx < h.x1 && cz > h.z0 && cz < h.z1);
			}
			if (!inside) {
				add_floor(r_out, xs[i], zs[j], xs[i + 1], zs[j + 1], p_y);
			}
		}
	}
}

std::vector<NavTileKey> tiles_range(int32_t p_x0, int32_t p_z0, int32_t p_x1, int32_t p_z1) {
	std::vector<NavTileKey> out;
	for (int32_t x = p_x0; x <= p_x1; ++x) {
		for (int32_t z = p_z0; z <= p_z1; ++z) {
			out.push_back({ x, z });
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::shared_ptr<const NavWorld> bake(const std::vector<StaticTriangle> &p_tris, const std::vector<NavTileKey> &p_tiles, NavBakeOptions p_options = {}) {
	core::Result<std::shared_ptr<const NavWorld>> w = bake_nav_world(std::span<const StaticTriangle>(p_tris), p_tiles, p_options);
	REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
	return *w;
}

// Strongly connected components with walkable area (an off-mesh link poly is an SCC of its own with no area).
size_t ground_sccs(const NavComponents &p_c) {
	size_t n = 0;
	for (double a : p_c.area) {
		n += a > 0.0 ? 1 : 0;
	}
	return n;
}

// The component of the poly under (x, z) near height y (UINT32_MAX when none within 0.5 m horizontally).
NavPointCheck at(const NavWorld &p_world, const NavComponents &p_c, float p_x, float p_y, float p_z) {
	return nav_check_point(p_world, p_c, { p_x, p_y, p_z }, { 0.5f, 1.0f, 0.5f });
}

size_t polys_with_area(const NavWorld &p_world, uint8_t p_area) {
	const dtNavMesh &mesh = *p_world.impl().mesh;
	size_t n = 0;
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			n += tile->polys[pi].getArea() == p_area && tile->polys[pi].getType() == DT_POLYTYPE_GROUND ? 1 : 0;
		}
	}
	return n;
}

std::atomic<int> g_scratch{ 0 };

struct Scratch {
	fs::path base, root, protected_dir;
	Scratch() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		base = fs::path(tmp) /
				(L"openbf3-nav-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(g_scratch++));
		root = base / L"cache";
		protected_dir = base / L"install";
		fs::create_directories(protected_dir);
	}
	~Scratch() {
		std::error_code ec;
		fs::remove_all(base, ec);
	}
	core::CacheOptions options() const {
		core::CacheOptions o;
		o.root = root;
		o.guard.add_protected_root(protected_dir);
		return o;
	}
};

// The brute-force SCC of the same directed graph nav_components reads (every Detour link of every poly): u and v
// share a component when each reaches the other (BFS from every poly).
std::vector<uint32_t> brute_scc(const NavWorld &p_world) {
	const NavWorld::Impl &w = p_world.impl();
	const size_t n = w.polys;
	std::vector<std::vector<uint32_t>> adj(n);
	const dtNavMesh &mesh = *w.mesh;
	for (int ti = 0; ti < mesh.getMaxTiles(); ++ti) {
		const dtMeshTile *tile = mesh.getTile(ti);
		if (tile == nullptr || tile->header == nullptr) {
			continue;
		}
		for (int pi = 0; pi < tile->header->polyCount; ++pi) {
			const uint32_t g = w.poly_base[size_t(ti)] + uint32_t(pi);
			for (unsigned int l = tile->polys[pi].firstLink; l != DT_NULL_LINK; l = tile->links[l].next) {
				const uint32_t h = w.global(NavPolyRef(tile->links[l].ref));
				if (h != UINT32_MAX) {
					adj[g].push_back(h);
				}
			}
		}
	}
	std::vector<std::vector<uint8_t>> reach(n, std::vector<uint8_t>(n, 0));
	for (uint32_t s = 0; s < n; ++s) {
		std::vector<uint32_t> q{ s };
		reach[s][s] = 1;
		for (size_t i = 0; i < q.size(); ++i) {
			for (uint32_t v : adj[q[i]]) {
				if (!reach[s][v]) {
					reach[s][v] = 1;
					q.push_back(v);
				}
			}
		}
	}
	std::vector<uint32_t> comp(n, UINT32_MAX);
	uint32_t next = 0;
	for (uint32_t u = 0; u < n; ++u) {
		if (comp[u] != UINT32_MAX) {
			continue;
		}
		for (uint32_t v = u; v < n; ++v) {
			if (reach[u][v] && reach[v][u]) {
				comp[v] = next;
			}
		}
		++next;
	}
	return comp;
}

// Same partition (component ids may differ).
bool same_partition(const std::vector<uint32_t> &p_a, const std::vector<uint32_t> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	std::unordered_map<uint32_t, uint32_t> fa, fb;
	for (size_t i = 0; i < p_a.size(); ++i) {
		const auto x = fa.emplace(p_a[i], p_b[i]);
		const auto y = fb.emplace(p_b[i], p_a[i]);
		if (x.first->second != p_b[i] || y.first->second != p_a[i]) {
			return false;
		}
	}
	return true;
}

// A world of platforms at several heights on one floor: steps, ledges (drop links), a raised island with no way up.
std::vector<StaticTriangle> terraces() {
	std::vector<StaticTriangle> t;
	const std::vector<Rect> blocks{ { 8, 8, 24, 24 }, { 40, 8, 56, 24 }, { 72, 8, 88, 56 }, { 100, 30, 120, 50 } };
	add_ground(t, 0, 0, 128, 64, 0.0f, blocks);
	add_block(t, { 8, 0, 8 }, { 24, 3.0f, 24 }); // a 3 m block: drop links down, no way up
	add_block(t, { 40, 0, 8 }, { 56, 0.25f, 24 }); // a step: joined both ways
	add_block(t, { 72, 0, 8 }, { 88, 2.0f, 56 }); // a 2 m block
	add_block(t, { 100, 0, 30 }, { 120, 4.5f, 50 }); // 4.5 m: drops still within drop_max
	return t;
}

} // namespace

TEST_SUITE("sim-nav-unit") {
	TEST_CASE("sim.nav.unit: a floor over four tiles is one component; the seams join; the debug mesh covers it") {
		std::vector<StaticTriangle> t;
		add_floor(t, 0, 0, 128, 128, 0.0f);
		const auto tiles = tiles_range(0, 0, 1, 1);
		const auto w = bake(t, tiles);
		CHECK(w->stats().tiles == 4);
		CHECK(w->stats().drop_links == 0);
		const NavComponents c = nav_components(*w);
		CHECK(ground_sccs(c) == 1);
		CHECK(c.weak_count == 1);
		// Area: 128^2 minus the 0.5 m erosion along the outer edge, roughly.
		CHECK(c.area[c.largest] > 0.95 * 127.0 * 127.0);
		CHECK(c.area[c.largest] < 128.0 * 128.0);
		const NavPointCheck p = at(*w, c, 63.9f, 0.0f, 64.1f);
		CHECK(p.poly != 0);
		CHECK(p.component == c.largest);
		const NavDebugMesh dm = nav_debug_mesh(*w, &c);
		CHECK(!dm.triangles.empty());
		CHECK(dm.triangles.size() == dm.triangle_component.size() * 3);
		CHECK(dm.links.empty());
		// Each tile's input holds both slab triangles that reach it (the bbox gather).
		for (size_t i = 0; i < w->tile_count(); ++i) {
			CHECK(w->tile(i).input_triangles >= 2);
			CHECK(w->tile(i).polys > 0);
		}
	}

	TEST_CASE("sim.nav.unit: a 0.9 m door is closed with Recast's ceil radius (0.5 m) and open with round to nearest (0.25 m)") {
		std::vector<StaticTriangle> t;
		add_floor(t, 0, 0, 64, 64, 0.0f);
		// A 0.2 m wall at x = 32 across the tile, with a door z in [29.95, 30.85] (0.9 m).
		add_box(t, { 31.9f, 0.0f, -1.0f }, { 32.1f, 3.0f, 29.95f });
		add_box(t, { 31.9f, 0.0f, 30.85f }, { 32.1f, 3.0f, 65.0f });
		const auto tiles = tiles_range(0, 0, 0, 0);
		NavBakeOptions o;
		o.params.drop_links = false;
		const auto closed = bake(t, tiles, o);
		const NavComponents cc = nav_components(*closed);
		const NavPointCheck a = at(*closed, cc, 16, 0, 30.4f), b = at(*closed, cc, 48, 0, 30.4f);
		REQUIRE(a.poly != 0);
		REQUIRE(b.poly != 0);
		CHECK(a.weak_component != b.weak_component);
		o.params.radius_round_up = false;
		const auto open = bake(t, tiles, o);
		const NavComponents oc = nav_components(*open);
		const NavPointCheck c = at(*open, oc, 16, 0, 30.4f), d = at(*open, oc, 48, 0, 30.4f);
		REQUIRE(c.poly != 0);
		REQUIRE(d.poly != 0);
		CHECK(c.component == d.component);
		CHECK(ground_sccs(oc) == 1);
	}

	TEST_CASE("sim.nav.unit: a 1.5 m ceiling gives crouch area; the slab top is walk area") {
		std::vector<StaticTriangle> t;
		add_floor(t, 0, 0, 64, 64, 0.0f);
		add_box(t, { 10, 1.5f, 10 }, { 30, 1.7f, 50 }); // room under it: 1.5 m (crouch 1.15 <= room < stand 1.8)
		const auto w = bake(t, tiles_range(0, 0, 0, 0));
		CHECK(polys_with_area(*w, kNavAreaCrouch) > 0);
		CHECK(polys_with_area(*w, kNavAreaWalk) > 0);
		const NavComponents c = nav_components(*w);
		const NavPointCheck under = at(*w, c, 20, 0, 30);
		REQUIRE(under.poly != 0);
		uint8_t area = 0;
		uint16_t flags = 0;
		nav_poly_area_flags(*w, under.poly, &area, &flags);
		CHECK(area == kNavAreaCrouch);
		CHECK(flags == nav_flags::kCrouch);
		const NavPointCheck top = at(*w, c, 20, 1.7f, 30);
		REQUIRE(top.poly != 0);
		nav_poly_area_flags(*w, top.poly, &area, &flags);
		CHECK(area == kNavAreaWalk);
		// The crouch passage joins the open floor (one weak and one strong component, apart from the slab top).
		const NavPointCheck open = at(*w, c, 50, 0, 30);
		CHECK(open.component == under.component);
	}

	TEST_CASE("sim.nav.unit: a 0.29 m step joins; a 0.45 m step does not (a drop link down, no way up)") {
		for (const float h : { 0.29f, 0.45f }) {
			std::vector<StaticTriangle> t;
			add_ground(t, 0, 0, 64, 64, 0.0f, { { 30, 10, 54, 54 } });
			add_block(t, { 30, 0, 10 }, { 54, h, 54 });
			const auto w = bake(t, tiles_range(0, 0, 0, 0));
			const NavComponents c = nav_components(*w);
			const NavPointCheck lo = at(*w, c, 10, 0, 30), hi = at(*w, c, 42, h, 30);
			REQUIRE(lo.poly != 0);
			REQUIRE(hi.poly != 0);
			INFO("step " << h);
			if (h < 0.3f) {
				CHECK(lo.component == hi.component);
				CHECK(ground_sccs(c) == 1);
				CHECK(w->stats().drop_links == 0);
			} else {
				CHECK(lo.component != hi.component);
				CHECK(lo.weak_component == hi.weak_component); // the drop links join them one way
				CHECK(w->stats().drop_links > 0);
			}
		}
	}

	TEST_CASE("sim.nav.unit: a 3 m ledge gets drop links, swept clear on the static world; two SCCs, one weak component; a wall blocks a link") {
		std::vector<StaticTriangle> t;
		add_ground(t, 0, 0, 64, 64, 0.0f, { { 20, 20, 40, 40 } });
		add_block(t, { 20, 0, 20 }, { 40, 3.0f, 40 });
		const auto tiles = tiles_range(0, 0, 0, 0);
		const auto w = bake(t, tiles);
		CHECK(w->stats().drop_links >= 1);
		CHECK(w->stats().link_sweeps == 0); // no link_shapes: every candidate kept (synthetic only)
		const NavComponents c = nav_components(*w);
		CHECK(ground_sccs(c) == 2);
		CHECK(c.weak_count == 1);
		const NavPointCheck top = at(*w, c, 30, 3, 30), ground = at(*w, c, 10, 0, 10);
		CHECK(top.component != ground.component);
		CHECK(top.weak_component == ground.weak_component);
		CHECK(c.largest == ground.component);

		// With the link checks on a static world built from the same triangles: the same links survive...
		core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(std::span<const StaticTriangle>(t));
		REQUIRE(shapes.ok());
		NavBakeOptions o;
		o.link_shapes = *shapes;
		o.threads = 4;
		const auto checked = bake(t, tiles, o);
		CHECK(checked->stats().drop_links == w->stats().drop_links);
		CHECK(checked->stats().link_sweeps == 2 * checked->stats().link_candidates);
		CHECK(checked->stats().links_rejected == 0);
		CHECK(checked->stats().link_worlds == 1);
		// ...and a Collision bar 1 m high hanging just beyond the -x edge blocks that side's links (the out sweep hits it).
		std::vector<StaticTriangle> railed = t;
		add_box(railed, { 19.6f, 3.0f, 20.0f }, { 19.7f, 4.0f, 40.0f });
		core::Result<std::shared_ptr<const StaticWorldShapes>> rail_shapes = build_static_world(std::span<const StaticTriangle>(railed));
		REQUIRE(rail_shapes.ok());
		o.link_shapes = *rail_shapes;
		NavBakeOptions unchecked;
		const auto rail_unchecked = bake(railed, tiles, unchecked);
		const auto rail_checked = bake(railed, tiles, o);
		CHECK(rail_checked->stats().links_rejected > 0);
		CHECK(rail_checked->stats().drop_links < rail_unchecked->stats().drop_links);
		CHECK(rail_checked->stats().drop_links > 0); // the other three sides keep theirs
	}

	TEST_CASE("sim.nav.unit: a 200 m slab filed under one tile walks in every tile it covers; the input equals the cooked world (no world_only)") {
		// Two triangles over x, z in [-100, 100]: the soup files each under its centroid's tile ((-1, -1) and (0, 0)),
		// so tile (1, -2) sees them only through the bbox gather (revision 1's 3 x 3 gather would leave it empty).
		std::vector<StaticTriangle> t;
		add_floor(t, -100, -100, 100, 100, 0.0f);
		const auto tiles = tiles_range(-2, -2, 1, 1);
		const auto w = bake(t, tiles);
		CHECK(w->stats().tiles == 16);
		CHECK(w->stats().tiles_empty == 0);
		CHECK(w->stats().far_triangles > 0);
		const NavComponents c = nav_components(*w);
		CHECK(ground_sccs(c) == 1);
		const NavPointCheck far_point = at(*w, c, 90.0f, 0.0f, -90.0f); // tile (1, -2)
		CHECK(far_point.poly != 0);
		core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(std::span<const StaticTriangle>(t));
		REQUIRE(shapes.ok());
		core::Result<std::vector<NavTileInput>> inputs = nav_tile_inputs_triangles(t, tiles, NavParams(), query_mask::kMovement, 2);
		REQUIRE(inputs.ok());
		for (const NavTileInput &in : *inputs) {
			core::Result<NavInputCheck> chk = nav_check_input_against_world(**shapes, in, NavParams());
			REQUIRE(chk.ok());
			INFO("tile " << in.tile[0] << "," << in.tile[1]);
			CHECK(chk->world_only == 0);
			CHECK(chk->nav_only == chk->nav_only_degenerate);
			CHECK(chk->matched == in.layers.size());
			CHECK(chk->max_corner_error < 0.001f);
		}
	}

	TEST_CASE("sim.nav.unit: the input border: a wall 2.0 m past the tile edge is in the tile's input digest, one 2.2 m past is not") {
		std::vector<StaticTriangle> base;
		add_floor(base, 0, 0, 64, 64, 0.0f);
		add_floor(base, 64, 0, 128, 64, -3.0f);
		const auto digest_of = [](const std::vector<StaticTriangle> &p_t) {
			core::Result<std::vector<NavTileInput>> in = nav_tile_inputs_triangles(p_t, std::vector<NavTileKey>{ { 0, 0 } }, NavParams(), query_mask::kMovement, 1);
			REQUIRE(in.ok());
			return (*in)[0].digest;
		};
		CHECK(nav_input_border(NavParams()) == doctest::Approx(2.1f));
		CHECK(nav_input_border(NavParams()) > float(nav_recast_border_cells(NavParams())) * 0.25f); // the sweep term is the larger one
		const core::Sha256Digest d0 = digest_of(base);
		std::vector<StaticTriangle> near_wall = base, far_wall = base;
		add_box(near_wall, { 66.0f, -3.0f, 10.0f }, { 66.1f, 1.0f, 50.0f }); // its box starts 2.0 m past the edge
		add_box(far_wall, { 66.2f, -3.0f, 10.0f }, { 66.3f, 1.0f, 50.0f });
		CHECK(!(digest_of(near_wall) == d0));
		CHECK(digest_of(far_wall) == d0);
		// The tile key is part of the digest: one slab is the whole input of two tiles, the digests differ.
		std::vector<StaticTriangle> slab;
		add_floor(slab, -10, -10, 138, 74, 0.0f);
		core::Result<std::vector<NavTileInput>> two =
				nav_tile_inputs_triangles(slab, std::vector<NavTileKey>{ { 0, 0 }, { 1, 0 } }, NavParams(), query_mask::kMovement, 1);
		REQUIRE(two.ok());
		CHECK((*two)[0].vertices == (*two)[1].vertices);
		CHECK(!((*two)[0].digest == (*two)[1].digest));
	}

	TEST_CASE("sim.nav.unit: the input set is the motor's: a Raycast box never enters the nav (kMovement), it blocks only with kAll") {
		std::vector<StaticTriangle> t;
		add_ground(t, 0, 0, 64, 64, 0.0f, { { 40.1f, 40.1f, 43.9f, 43.9f } });
		add_block(t, { 20.1f, 0, -2 }, { 21.9f, 3, 66 }, StaticLayer::Raycast); // a see-through wall across the tile
		add_block(t, { 40.1f, 0, 40.1f }, { 43.9f, 2, 43.9f }, StaticLayer::Character); // a PLAYERCLIP block
		core::Result<std::vector<NavTileInput>> in = nav_tile_inputs_triangles(t, std::vector<NavTileKey>{ { 0, 0 } }, NavParams(), query_mask::kMovement, 1);
		REQUIRE(in.ok());
		size_t raycast = 0, character = 0;
		for (uint8_t l : (*in)[0].layers) {
			raycast += l == uint8_t(StaticLayer::Raycast) ? 1 : 0;
			character += l == uint8_t(StaticLayer::Character) ? 1 : 0;
		}
		CHECK(raycast == 0);
		CHECK(character == 10);
		const auto tiles = tiles_range(0, 0, 0, 0);
		const auto movement = bake(t, tiles);
		const NavComponents mc = nav_components(*movement);
		CHECK(at(*movement, mc, 10, 0, 32).component == at(*movement, mc, 32, 0, 32).component);
		CHECK(at(*movement, mc, 42, 0, 42).poly == 0); // under the PLAYERCLIP block: nothing
		NavBakeOptions all;
		all.layer_mask = query_mask::kAll;
		const auto with_raycast = bake(t, tiles, all);
		const NavComponents ac = nav_components(*with_raycast);
		// With Raycast in the input the wall splits the floor: its top (3 m) drops to both sides, so the two halves
		// are one weak component but two SCCs.
		CHECK(at(*with_raycast, ac, 10, 0, 32).component != at(*with_raycast, ac, 32, 0, 32).component);
		// The movement input matches the cooked world on the movement layers, and the Raycast shape is not in it.
		core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(std::span<const StaticTriangle>(t));
		REQUIRE(shapes.ok());
		core::Result<NavInputCheck> chk = nav_check_input_against_world(**shapes, (*in)[0], NavParams());
		REQUIRE(chk.ok());
		CHECK(chk->world_only == 0);
		CHECK(chk->nav_only == 0);
	}

	TEST_CASE("sim.nav.unit: determinism (bytes equal on 1, 8 and 16 lanes); the link-check pool on 16 lanes holds at most 16 worlds") {
		std::vector<StaticTriangle> t;
		add_floor(t, 0, 0, 256, 256, 0.0f);
		for (int i = 0; i < 16; ++i) {
			const float x = 8.0f + 64.0f * float(i % 4), z = 8.0f + 64.0f * float(i / 4);
			add_block(t, { x, 0, z }, { x + 12, 2.5f + 0.1f * float(i), z + 12 });
		}
		const auto tiles = tiles_range(0, 0, 3, 3);
		core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(std::span<const StaticTriangle>(t));
		REQUIRE(shapes.ok());
		NavBakeOptions o;
		o.link_shapes = *shapes;
		o.threads = 1;
		const auto a = bake(t, tiles, o);
		o.threads = 8;
		const auto b = bake(t, tiles, o);
		o.threads = 16;
		const auto c = bake(t, tiles, o);
		REQUIRE(a->tile_count() == b->tile_count());
		for (size_t i = 0; i < a->tile_count(); ++i) {
			CHECK(a->impl().data[i] == b->impl().data[i]);
			CHECK(a->impl().data[i] == c->impl().data[i]);
		}
		CHECK(a->stats().link_worlds == 1);
		CHECK(c->stats().link_worlds >= 1);
		CHECK(c->stats().link_worlds <= 16);
		CHECK(c->stats().drop_links > 0);
		CHECK(c->stats().drop_links == a->stats().drop_links);
		std::printf("sim.nav.unit: 16 tiles, %zu polys, %zu drop links, %zu link worlds on 16 lanes, bake %.3f s (link cpu %.3f s, setup %.3f s)\n",
				c->stats().polys, c->stats().drop_links, c->stats().link_worlds, c->stats().seconds_bake, c->stats().seconds_link_cpu,
				c->stats().seconds_link_setup);
	}

	TEST_CASE("sim.nav.unit: nav_components equals a brute-force SCC on a terraced world; weak ids are compact") {
		const std::vector<StaticTriangle> t = terraces();
		const auto w = bake(t, tiles_range(0, 0, 1, 0));
		const NavComponents c = nav_components(*w);
		const std::vector<uint32_t> brute = brute_scc(*w);
		CHECK(same_partition(c.component, brute));
		CHECK(c.weak_count == 1);
		std::set<uint32_t> weak(c.weak_component.begin(), c.weak_component.end());
		CHECK(weak.size() == c.weak_count);
		CHECK(*weak.rbegin() == c.weak_count - 1);
		// The 3 m, 2 m and 4.5 m blocks are separate SCCs (drops only); the 0.25 m step is part of the floor.
		CHECK(ground_sccs(c) == 4);
		const NavPointCheck ground = at(*w, c, 64, 0, 60), step = at(*w, c, 48, 0.25f, 16);
		CHECK(ground.component == step.component);
		CHECK(c.largest == ground.component);
		CHECK(nav_poly_component(*w, c, ground.poly) == ground.component);
		CHECK(nav_poly_component(*w, c, 0) == UINT32_MAX);
	}

	TEST_CASE("sim.nav.unit: NavQuery paths (sliced, budgeted), random points by component, partial results; NavCorridor follows and reports leaving") {
		const std::vector<StaticTriangle> t = terraces();
		const auto w = bake(t, tiles_range(0, 0, 1, 0));
		const NavComponents c = nav_components(*w);
		core::Result<std::unique_ptr<NavQuery>> q = NavQuery::create(w);
		REQUIRE(q.ok());
		NavQuery &query = **q;
		NavPolyRef a = 0, b = 0;
		Vec3 pa{}, pb{};
		REQUIRE(query.nearest({ 4, 0, 60 }, { 2, 2, 2 }, &a, &pa));
		REQUIRE(query.nearest({ 124, 0, 4 }, { 2, 2, 2 }, &b, &pb));
		CHECK(query.begin_path(a, pa, b, pb) == NavPathStatus::InProgress);
		NavPathStatus st = NavPathStatus::InProgress;
		uint32_t total = 0, steps = 0;
		while (st == NavPathStatus::InProgress && steps < 10000) {
			uint32_t it = 0;
			st = query.continue_path(4, &it);
			CHECK(it <= 4);
			total += it;
			++steps;
		}
		CHECK(st == NavPathStatus::Done);
		std::vector<NavPolyRef> polys;
		CHECK(query.finish_path(polys) == NavPathStatus::Done);
		REQUIRE(!polys.empty());
		CHECK(polys.front() == a);
		CHECK(polys.back() == b);
		CHECK(total > 0);
		NavCorridor corridor;
		corridor.reset(polys, pa, pb);
		NavCorner corners[kMaxNavCorners];
		const uint32_t n = corridor.corners(query, corners, kMaxNavCorners);
		CHECK(n >= 1);
		CHECK(corridor.move_position(query, { pa[0] + 0.5f, pa[1], pa[2] - 0.5f }));
		CHECK(!corridor.move_position(query, { 64, 0, 200 })); // far off the corridor: left it
		// The 3 m block's top is another SCC: a path from the floor up to it is partial.
		NavPolyRef top = 0;
		Vec3 ptop{};
		REQUIRE(query.nearest({ 16, 3, 16 }, { 1, 1, 1 }, &top, &ptop));
		CHECK(nav_poly_component(*w, c, top) != c.largest);
		CHECK(query.begin_path(a, pa, top, ptop) == NavPathStatus::InProgress);
		st = NavPathStatus::InProgress;
		for (int i = 0; i < 1000 && st == NavPathStatus::InProgress; ++i) {
			st = query.continue_path(64, nullptr);
		}
		CHECK(query.finish_path(polys) == NavPathStatus::Partial);
		// From the top down is possible (drop links), and the corners show the drop.
		CHECK(query.begin_path(top, ptop, a, pa) == NavPathStatus::InProgress);
		st = NavPathStatus::InProgress;
		for (int i = 0; i < 1000 && st == NavPathStatus::InProgress; ++i) {
			st = query.continue_path(64, nullptr);
		}
		CHECK(query.finish_path(polys) == NavPathStatus::Done);
		bool has_drop = false;
		corridor.reset(polys, ptop, pa);
		const uint32_t m = corridor.corners(query, corners, kMaxNavCorners);
		for (uint32_t i = 0; i < m; ++i) {
			has_drop = has_drop || (corners[i].flags & nav_flags::kDrop) != 0;
		}
		CHECK(has_drop);
		// Random points stay in their component and are deterministic per seed.
		uint64_t rng = 7, rng2 = 7;
		for (int i = 0; i < 200; ++i) {
			NavPolyRef p = 0, p2 = 0;
			Vec3 v{}, v2{};
			REQUIRE(query.random_point(rng, c, c.largest, &p, &v));
			REQUIRE(query.random_point(rng2, c, c.largest, &p2, &v2));
			CHECK(p == p2);
			CHECK(nav_poly_component(*w, c, p) == c.largest);
		}
		CHECK(!query.random_point(rng, c, 9999, nullptr, nullptr));
		CHECK(query.stats().paths_started == 3);
		CHECK(query.stats().paths_partial == 1);
	}

	TEST_CASE("sim.nav.unit: tile blobs round trip; every truncation, trailing bytes and 2,000 mutated blobs are refused or load and query cleanly") {
		const std::vector<StaticTriangle> t = terraces();
		const auto w = bake(t, tiles_range(0, 0, 1, 0));
		std::vector<std::vector<uint8_t>> blobs;
		for (size_t i = 0; i < w->tile_count(); ++i) {
			blobs.push_back(encode_nav_tile_blob(*w, i));
		}
		std::vector<std::span<const uint8_t>> spans(blobs.begin(), blobs.end());
		core::Result<std::shared_ptr<const NavWorld>> back = decode_nav_tile_blobs(spans, w->params());
		REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
		REQUIRE((*back)->tile_count() == w->tile_count());
		CHECK((*back)->poly_count() == w->poly_count());
		for (size_t i = 0; i < w->tile_count(); ++i) {
			CHECK((*back)->impl().data[i] == w->impl().data[i]);
			CHECK((*back)->tile(i).input == w->tile(i).input);
		}
		const std::vector<uint8_t> &blob = blobs[0];
		size_t tried = 0, refused = 0;
		for (size_t len = 0; len < blob.size(); len += (len < 1024 ? 1 : 13)) {
			const std::span<const uint8_t> cut(blob.data(), len);
			NavTileInfo info;
			std::vector<uint8_t> data;
			++tried;
			refused += decode_nav_tile(cut, w->params(), info, data).ok() ? 0 : 1;
		}
		CHECK(refused == tried);
		std::vector<uint8_t> longer = blob;
		longer.push_back(0);
		{
			NavTileInfo info;
			std::vector<uint8_t> data;
			CHECK(!decode_nav_tile(longer, w->params(), info, data).ok());
		}
		// Mutations: random bytes anywhere; whatever loads must query cleanly (also under asan).
		std::mt19937_64 rng(12345);
		size_t loaded = 0, rejected = 0;
		for (int round = 0; round < 2000; ++round) {
			std::vector<uint8_t> m = blobs[size_t(round) % blobs.size()];
			const int flips = 1 + int(rng() % 8);
			for (int f = 0; f < flips; ++f) {
				const uint8_t value = uint8_t(rng());
				const size_t pos = size_t(rng() % m.size());
				m[pos] = value;
			}
			const std::span<const uint8_t> one[1] = { m };
			core::Result<std::shared_ptr<const NavWorld>> d = decode_nav_tile_blobs(one, w->params());
			if (!d) {
				++rejected;
				continue;
			}
			++loaded;
			const NavComponents c = nav_components(**d);
			core::Result<std::unique_ptr<NavQuery>> q = NavQuery::create(*d);
			if (!q) {
				continue;
			}
			uint64_t r = uint64_t(round);
			NavPolyRef p0 = 0, p1 = 0;
			Vec3 v0{}, v1{};
			if ((*q)->random_point(r, c, UINT32_MAX, &p0, &v0) && (*q)->random_point(r, c, UINT32_MAX, &p1, &v1)) {
				if ((*q)->begin_path(p0, v0, p1, v1) == NavPathStatus::InProgress) {
					for (int i = 0; i < 64 && (*q)->continue_path(256, nullptr) == NavPathStatus::InProgress; ++i) {
					}
					std::vector<NavPolyRef> polys;
					(*q)->finish_path(polys);
					NavCorridor corridor;
					corridor.reset(polys, v0, v1);
					NavCorner corners[kMaxNavCorners];
					(void)corridor.corners(**q, corners, kMaxNavCorners);
				}
			}
			(void)nav_debug_mesh(**d, &c);
		}
		std::printf("sim.nav.unit: 2,000 mutated tile blobs: %zu refused, %zu loaded and queried\n", rejected, loaded);
		CHECK(rejected + loaded == 2000);
		CHECK(rejected > 0);
	}

	TEST_CASE("sim.nav.unit: hostile inputs fail with an error: 5,000 tile-wide triangles, 2^21 + 1 triangles in one tile; a 2 km tall tile bakes") {
		{
			std::vector<StaticTriangle> t;
			for (int i = 0; i < 5000; ++i) {
				const float y = 0.01f * float(i);
				StaticTriangle tri;
				tri.v = { Vec3{ -1, y, -1 }, Vec3{ -1, y, 200 }, Vec3{ 200, y, -1 } };
				t.push_back(tri);
			}
			core::Result<std::shared_ptr<const NavWorld>> w = bake_nav_world(std::span<const StaticTriangle>(t), tiles_range(0, 0, 0, 0));
			REQUIRE(!w.ok());
			CHECK(w.error().message.find("raster cells") != std::string::npos);
		}
		{
			std::vector<StaticTriangle> t;
			add_floor(t, 0, 0, 64, 64, 0.0f);
			add_floor(t, 10, 10, 20, 20, 2000.0f);
			NavBakeOptions o;
			o.params.drop_links = false;
			core::Result<std::shared_ptr<const NavWorld>> w = bake_nav_world(std::span<const StaticTriangle>(t), tiles_range(0, 0, 0, 0), o);
			REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
			CHECK((*w)->stats().tiles == 1);
		}
		{
			std::vector<StaticTriangle> t(kMaxNavTileInputTriangles + 1);
			for (size_t i = 0; i < t.size(); ++i) {
				const float x = 1.0f + float(i % 2000) * 0.03f, z = 1.0f + float(i / 2000) * 0.05f;
				t[i].v = { Vec3{ x, 0, z }, Vec3{ x, 0, z + 0.01f }, Vec3{ x + 0.01f, 0, z } };
			}
			core::Result<std::vector<NavTileInput>> in = nav_tile_inputs_triangles(t, std::vector<NavTileKey>{ { 0, 0 } }, NavParams(), query_mask::kMovement, 1);
			REQUIRE(!in.ok());
			CHECK(in.error().message.find("more than") != std::string::npos);
		}
		std::vector<StaticTriangle> t;
		add_floor(t, 0, 0, 64, 64, 0.0f);
		CHECK(!bake_nav_world(std::span<const StaticTriangle>(t), std::vector<NavTileKey>{ { 1, 0 }, { 0, 0 } }).ok()); // unsorted
		CHECK(!bake_nav_world(std::span<const StaticTriangle>(t), std::vector<NavTileKey>{ { 1 << 22, 0 } }).ok()); // out of range
		NavBakeOptions bad;
		bad.params.agent_climb = 0.3004f; // 3.004 cells: not whole
		CHECK(!bake_nav_world(std::span<const StaticTriangle>(t), tiles_range(0, 0, 0, 0), bad).ok());
		bad.params = NavParams();
		bad.params.cell_size = 0.3f; // does not divide 64 m
		CHECK(!bake_nav_world(std::span<const StaticTriangle>(t), tiles_range(0, 0, 0, 0), bad).ok());
		CHECK(nav_cells(NavParams()).walkable_climb == 3);
		CHECK(nav_cells(NavParams()).walkable_radius == 2);
		NavParams nearest;
		nearest.radius_round_up = false;
		CHECK(nav_cells(nearest).walkable_radius == 1);
	}

	TEST_CASE("sim.nav.unit: cache keys change with the input, the params and the link check; cold then warm tiles on a scratch root") {
		core::Sha256Digest d0{}, d1{};
		d1.bytes[0] = 1;
		const NavParams p;
		const auto key = [](const core::CacheKeyInputs &p_in) {
			const std::optional<core::CacheKey> k = core::make_cache_key(p_in);
			REQUIRE(k.has_value());
			return k->hex();
		};
		const std::string k0 = key(nav_tile_key_inputs(d0, p, true));
		CHECK(key(nav_tile_key_inputs(d1, p, true)) != k0);
		CHECK(key(nav_tile_key_inputs(d0, p, false)) != k0);
		NavParams q = p;
		q.cell_height = 0.2f;
		CHECK(key(nav_tile_key_inputs(d0, q, true)) != k0);
		q = p;
		q.radius_round_up = false;
		CHECK(key(nav_tile_key_inputs(d0, q, true)) != k0);
		q = p;
		q.drop_max = 4.0f;
		CHECK(key(nav_tile_key_inputs(d0, q, true)) != k0);
		q = p;
		q.partition = NavPartition::Monotone;
		CHECK(key(nav_tile_key_inputs(d0, q, true)) != k0);
		CHECK(nav_tile_key_inputs(d0, p, true).params.find(recast_build_id()) != std::string::npos);
		CHECK(recast_build_id() == "recast=1.6.0;polyref64=1;verts_per_poly=6");

		Scratch scratch;
		const std::vector<StaticTriangle> t = terraces();
		const auto tiles = tiles_range(0, 0, 1, 0);
		std::vector<std::vector<uint8_t>> first;
		for (int pass = 0; pass < 2; ++pass) {
			core::Cache cache;
			REQUIRE(cache.open(scratch.options()) == core::CacheStatus::Ok);
			core::Result<std::vector<NavTileInput>> in = nav_tile_inputs_triangles(t, tiles, p, query_mask::kMovement, 2);
			REQUIRE(in.ok());
			bool stored = false;
			NavBakeOptions o;
			core::Result<std::shared_ptr<const NavWorld>> w = bake_nav_inputs(std::move(*in), o, NavStats(), &cache, &stored);
			REQUIRE(w.ok());
			CHECK(stored);
			CHECK((*w)->stats().tiles_hit == (pass == 0 ? 0u : 2u));
			CHECK((*w)->stats().tiles_baked == (pass == 0 ? 2u : 0u));
			if (pass == 0) {
				first = (*w)->impl().data;
			} else {
				CHECK((*w)->impl().data == first);
			}
			cache.close();
		}
	}
}
