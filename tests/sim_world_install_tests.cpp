// sim.world (install): Metro TDM's static world from the retail install (read-only) through a scratch cache
// (docs/design/m1.md §5, §14). Exits 77 (skipped) without an install.
//   * cold then warm: the tile list and per-tile, per-layer triangle counts equal the soup's (969,031 collision
//     + 142,908 raycast + 2,232 character); the warm path builds no soup (its progress never leaves Mount) and
//     restores every tile; cold and warm give byte-identical tile blobs and bit-identical answers to 1,000 seeded
//     rays; a second mode of the level (ConquestLarge0) takes the tiles it shares from the cache;
//   * coverage (§5.5): every EBX partition, res and chunk the soup read is an entry of a bundle whose manifest the
//     source digest hashed;
//   * parity: CollisionWorld ray / capsule overlap / box overlap / capsule sweep answers at sampled spots near the
//     geometry equal a brute-force reference over the soup triangles (double precision, no tree), with the
//     tolerances the Jolt build implies (vertex quantisation, the box's convex radius); ambiguous samples (within
//     the tolerance of a contact) are counted and bounded, mismatches must be 0;
//   * memory figures (§5.4) and the install snapshot (install.unchanged).
#include <doctest/doctest.h>

#include "bf3/core/bundle_manifest.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ids.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/source_digest.h"
#include "bf3/core/level/world.h"
#include "bf3/core/superbundle_mount.h"
#include "sim_world_support.h"
#include "test_support.h"

#include "collision_world_internal.h" // the body count of a CollisionWorld (memory figures)

#include <windows.h>
// psapi after windows.h
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

using namespace sim_world_test;

namespace {

namespace fs = std::filesystem;

constexpr const char *kLevel = "MP_Subway";
constexpr const char *kMode = "TeamDeathMatch0";

double mib(uint64_t p_bytes) {
	return double(p_bytes) / (1024.0 * 1024.0);
}

uint64_t peak_working_set() {
	PROCESS_MEMORY_COUNTERS pmc{};
	pmc.cb = sizeof(pmc);
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
	return pmc.PeakWorkingSetSize;
}

// A short scratch root: %TEMP%\o3w-<pid>-<tick> (entry paths stay well under MAX_PATH).
struct Scratch {
	fs::path root;
	Scratch() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		root = fs::path(tmp) / (L"o3w-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
	}
	~Scratch() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}
};

// ---- the brute-force reference (double precision over the soup triangles) --------------------------------------

struct D3 {
	double x = 0, y = 0, z = 0;
};
D3 d3(const Vec3 &p_v) {
	return { p_v[0], p_v[1], p_v[2] };
}
D3 operator+(D3 a, D3 b) {
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}
D3 operator-(D3 a, D3 b) {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}
D3 operator*(D3 a, double s) {
	return { a.x * s, a.y * s, a.z * s };
}
double dot(D3 a, D3 b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
D3 cross(D3 a, D3 b) {
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
double len(D3 a) {
	return std::sqrt(dot(a, a));
}

struct RefTriangle {
	std::array<D3, 3> v;
	D3 lo, hi; // bounds
	SurfaceTag tag;
	uint32_t layer = 0;
	uint32_t tile = 0;
};

struct Reference {
	std::vector<RefTriangle> triangles;
	struct Tile {
		D3 lo, hi;
		size_t first = 0, count = 0;
	};
	std::vector<Tile> tiles;
};

Reference make_reference(const level::CollisionSoup &p_soup) {
	Reference r;
	for (size_t t = 0; t < p_soup.tiles.size(); ++t) {
		const level::SoupTile &tile = p_soup.tiles[t];
		Reference::Tile rt;
		rt.first = r.triangles.size();
		rt.lo = d3(tile.min);
		rt.hi = d3(tile.max);
		for (const level::SoupTriangle &s : tile.triangles) {
			RefTriangle tri;
			for (size_t k = 0; k < 3; ++k) {
				tri.v[k] = d3(s.v[k]);
			}
			tri.lo = { std::min({ tri.v[0].x, tri.v[1].x, tri.v[2].x }), std::min({ tri.v[0].y, tri.v[1].y, tri.v[2].y }),
				std::min({ tri.v[0].z, tri.v[1].z, tri.v[2].z }) };
			tri.hi = { std::max({ tri.v[0].x, tri.v[1].x, tri.v[2].x }), std::max({ tri.v[0].y, tri.v[1].y, tri.v[2].y }),
				std::max({ tri.v[0].z, tri.v[1].z, tri.v[2].z }) };
			tri.tag.material = s.material;
			tri.tag.surface = s.surface;
			tri.tag.body = s.body;
			tri.tag.flags = s.flags;
			tri.layer = s.body;
			tri.tile = uint32_t(t);
			r.triangles.push_back(tri);
		}
		rt.count = r.triangles.size() - rt.first;
		r.tiles.push_back(rt);
	}
	return r;
}

bool boxes_touch(D3 a_lo, D3 a_hi, D3 b_lo, D3 b_hi, double p_pad) {
	return a_lo.x <= b_hi.x + p_pad && b_lo.x <= a_hi.x + p_pad && a_lo.y <= b_hi.y + p_pad && b_lo.y <= a_hi.y + p_pad && a_lo.z <= b_hi.z + p_pad &&
			b_lo.z <= a_hi.z + p_pad;
}

// Calls p_visit for every triangle of p_mask whose bounds touch [lo, hi] padded.
template <class F>
void for_candidates(const Reference &p_ref, uint32_t p_mask, D3 p_lo, D3 p_hi, double p_pad, F &&p_visit) {
	for (const Reference::Tile &tile : p_ref.tiles) {
		if (!boxes_touch(tile.lo, tile.hi, p_lo, p_hi, p_pad)) {
			continue;
		}
		for (size_t i = tile.first; i < tile.first + tile.count; ++i) {
			const RefTriangle &t = p_ref.triangles[i];
			if (((p_mask >> t.layer) & 1u) == 0 || !boxes_touch(t.lo, t.hi, p_lo, p_hi, p_pad)) {
				continue;
			}
			p_visit(t);
		}
	}
}

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
D3 closest_on_triangle(D3 p, D3 a, D3 b, D3 c) {
	const D3 ab = b - a, ac = c - a, ap = p - a;
	const double d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) {
		return a;
	}
	const D3 bp = p - b;
	const double d3v = dot(ab, bp), d4 = dot(ac, bp);
	if (d3v >= 0 && d4 <= d3v) {
		return b;
	}
	const double vc = d1 * d4 - d3v * d2;
	if (vc <= 0 && d1 >= 0 && d3v <= 0) {
		return a + ab * (d1 / (d1 - d3v));
	}
	const D3 cp = p - c;
	const double d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) {
		return c;
	}
	const double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) {
		return a + ac * (d2 / (d2 - d6));
	}
	const double va = d3v * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3v) >= 0 && (d5 - d6) >= 0) {
		return b + (c - b) * ((d4 - d3v) / ((d4 - d3v) + (d5 - d6)));
	}
	const double denom = 1.0 / (va + vb + vc);
	return a + ab * (vb * denom) + ac * (vc * denom);
}

// Squared distance between segments p1q1 and p2q2 (Ericson 5.1.9).
double segment_segment_sq(D3 p1, D3 q1, D3 p2, D3 q2) {
	const D3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
	const double a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
	double s = 0, t = 0;
	if (a <= 1e-18 && e <= 1e-18) {
		return dot(r, r);
	}
	if (a <= 1e-18) {
		t = std::clamp(f / e, 0.0, 1.0);
	} else {
		const double c = dot(d1, r);
		if (e <= 1e-18) {
			s = std::clamp(-c / a, 0.0, 1.0);
		} else {
			const double b = dot(d1, d2), denom = a * e - b * b;
			s = denom > 1e-18 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
			t = (b * s + f) / e;
			if (t < 0) {
				t = 0;
				s = std::clamp(-c / a, 0.0, 1.0);
			} else if (t > 1) {
				t = 1;
				s = std::clamp((b - c) / a, 0.0, 1.0);
			}
		}
	}
	const D3 c1 = p1 + d1 * s, c2 = p2 + d2 * t;
	return dot(c1 - c2, c1 - c2);
}

// Two-sided segment / triangle intersection parameter (Moller-Trumbore); nullopt when none.
std::optional<double> segment_hits_triangle(D3 p_origin, D3 p_delta, const RefTriangle &p_t) {
	const D3 e1 = p_t.v[1] - p_t.v[0], e2 = p_t.v[2] - p_t.v[0];
	const D3 h = cross(p_delta, e2);
	const double det = dot(e1, h);
	if (std::fabs(det) < 1e-18) {
		return std::nullopt;
	}
	const double inv = 1.0 / det;
	const D3 s = p_origin - p_t.v[0];
	const double u = dot(s, h) * inv;
	if (u < 0 || u > 1) {
		return std::nullopt;
	}
	const D3 q = cross(s, e1);
	const double v = dot(p_delta, q) * inv;
	if (v < 0 || u + v > 1) {
		return std::nullopt;
	}
	const double t = dot(e2, q) * inv;
	if (t < 0 || t > 1) {
		return std::nullopt;
	}
	return t;
}

double segment_triangle_distance(D3 p, D3 q, const RefTriangle &t) {
	if (segment_hits_triangle(p, q - p, t)) {
		return 0.0;
	}
	double best = std::min(len(p - closest_on_triangle(p, t.v[0], t.v[1], t.v[2])), len(q - closest_on_triangle(q, t.v[0], t.v[1], t.v[2])));
	for (size_t k = 0; k < 3; ++k) {
		best = std::min(best, std::sqrt(segment_segment_sq(p, q, t.v[k], t.v[(k + 1) % 3])));
	}
	return best;
}

// A ray candidate: the parameter, the in-plane margin (metres from the hit point to the nearest edge, negative
// outside) and whether the ray grazes the plane.
struct RayCandidate {
	double t = 0;
	double margin = 0;
	bool grazing = false;
	const RefTriangle *tri = nullptr;
};

std::optional<RayCandidate> ray_candidate(D3 p_origin, D3 p_delta, const RefTriangle &p_t, double p_slack) {
	const D3 n = cross(p_t.v[1] - p_t.v[0], p_t.v[2] - p_t.v[0]);
	const double nl = len(n);
	const double denom = dot(n, p_delta);
	if (nl < 1e-12 || std::fabs(denom) < 1e-18) {
		return std::nullopt;
	}
	const double t = dot(n, p_t.v[0] - p_origin) / denom;
	const double length = len(p_delta);
	if (t < -p_slack / length || t > 1.0 + p_slack / length) {
		return std::nullopt;
	}
	const D3 x = p_origin + p_delta * t;
	double margin = 1e30;
	for (size_t k = 0; k < 3; ++k) {
		const D3 a = p_t.v[k], b = p_t.v[(k + 1) % 3];
		const D3 inward = cross(n, b - a); // points into the triangle for a counter-clockwise (a, b, c) about n
		const double il = len(inward);
		if (il < 1e-15) {
			return std::nullopt;
		}
		margin = std::min(margin, dot(x - a, inward) / il);
	}
	RayCandidate c;
	c.t = t;
	c.margin = margin;
	c.grazing = std::fabs(denom) / (nl * length) < 0.02; // within ~1.1 degrees of the plane
	c.tri = &p_t;
	return c;
}

// Capsule axis (bottom and top sphere centres) of an upright capsule with its feet at p_feet.
std::pair<D3, D3> capsule_axis(D3 p_feet, double p_radius, double p_height) {
	return { p_feet + D3{ 0, p_radius, 0 }, p_feet + D3{ 0, p_height - p_radius, 0 } };
}

// Smallest distance from the capsule axis (moved by p_offset) to any candidate triangle.
double capsule_distance(const std::vector<const RefTriangle *> &p_candidates, std::pair<D3, D3> p_axis, D3 p_offset) {
	double best = 1e30;
	for (const RefTriangle *t : p_candidates) {
		best = std::min(best, segment_triangle_distance(p_axis.first + p_offset, p_axis.second + p_offset, *t));
	}
	return best;
}

// First sweep parameter in [0, 1] at which the capsule axis comes within p_radius of a candidate triangle
// (conservative advancement per triangle: the distance changes by at most |delta| per unit of t, so no contact is
// stepped over); nullopt when none.
std::optional<double> first_contact(const std::vector<const RefTriangle *> &p_candidates, std::pair<D3, D3> p_axis, D3 p_delta, double p_radius) {
	const double speed = len(p_delta);
	std::optional<double> best;
	for (const RefTriangle *tri : p_candidates) {
		double t = 0;
		for (int i = 0; i < 400 && t <= 1.0; ++i) {
			const double gap = segment_triangle_distance(p_axis.first + p_delta * t, p_axis.second + p_delta * t, *tri) - p_radius;
			if (gap <= 1e-6) {
				if (!best || t < *best) {
					best = t;
				}
				break;
			}
			if (best && t >= *best) {
				break;
			}
			t += gap / speed;
		}
	}
	return best;
}

// Box (half extents, yaw about +Y) against a triangle: the largest separation over the 13 SAT axes (positive =
// separated by at least that much, negative = overlapping on every axis by at least its magnitude).
double box_triangle_separation(D3 p_center, D3 p_half, double p_yaw, const RefTriangle &p_t) {
	const double c = std::cos(p_yaw), s = std::sin(p_yaw);
	// Jolt's Quat::sRotation(+Y, yaw): x' = (c, 0, -s), z' = (s, 0, c).
	const std::array<D3, 3> axes{ D3{ c, 0, -s }, D3{ 0, 1, 0 }, D3{ s, 0, c } };
	const std::array<double, 3> h{ p_half.x, p_half.y, p_half.z };
	const std::array<D3, 3> v{ p_t.v[0] - p_center, p_t.v[1] - p_center, p_t.v[2] - p_center };
	const std::array<D3, 3> e{ v[1] - v[0], v[2] - v[1], v[0] - v[2] };
	std::vector<D3> tests(axes.begin(), axes.end());
	tests.push_back(cross(e[0], e[1]));
	for (const D3 &a : axes) {
		for (const D3 &edge : e) {
			tests.push_back(cross(a, edge));
		}
	}
	double worst = -1e30;
	for (const D3 &axis : tests) {
		const double al = len(axis);
		if (al < 1e-9) {
			continue;
		}
		const D3 u = axis * (1.0 / al);
		const double r = h[0] * std::fabs(dot(axes[0], u)) + h[1] * std::fabs(dot(axes[1], u)) + h[2] * std::fabs(dot(axes[2], u));
		const double p0 = dot(v[0], u), p1 = dot(v[1], u), p2 = dot(v[2], u);
		const double lo = std::min({ p0, p1, p2 }), hi = std::max({ p0, p1, p2 });
		worst = std::max(worst, std::max(lo - r, -r - hi));
	}
	return worst;
}

// ---- the shared Metro data (loaded once per process) ----------------------------------------------------------

struct Metro {
	core::InstallReader reader;
	core::InstallSnapshot before;
	std::shared_ptr<core::Install> install;
	std::unique_ptr<level::LoadedLevel> level;
	std::unique_ptr<level::World> world; // after `level`: destroyed first
	level::CollisionSoup soup;
	std::shared_ptr<const StaticWorldShapes> cold;
	std::shared_ptr<const StaticWorldShapes> warm;
	Scratch scratch;
	bool ok = false;
};

bool load_metro(Metro &r_m) {
	if (!r_m.reader.locate({})) {
		return false;
	}
	r_m.before = r_m.reader.snapshot();
	core::Result<std::shared_ptr<core::Install>> install = core::Install::open(r_m.reader);
	REQUIRE(install.ok());
	r_m.install = *install;
	core::Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(r_m.install, kLevel, kMode);
	REQUIRE(loaded.ok());
	r_m.level = std::move(*loaded);
	core::Result<std::unique_ptr<level::World>> world = level::World::traverse(*r_m.level);
	REQUIRE(world.ok());
	r_m.world = std::move(*world);
	core::Result<level::CollisionSoup> soup = level::build_collision_soup(*r_m.level, *r_m.world);
	REQUIRE(soup.ok());
	r_m.soup = std::move(*soup);
	r_m.ok = true;
	return true;
}

// Loaded by the first case that asks; destroyed at exit (the scratch root goes with it).
Metro *metro() {
	static Metro m;
	static const bool loaded = load_metro(m);
	(void)loaded;
	return &m;
}

core::CacheOptions cache_options(const Metro &p_m) {
	core::CacheOptions o;
	o.root = p_m.scratch.root;
	o.guard = p_m.reader.write_guard();
	return o;
}

} // namespace

TEST_SUITE("sim-world") {
	TEST_CASE("sim.world: Metro cold then warm through a scratch cache: the soup's tiles, no soup when warm, identical shapes") {
		Metro *m = metro();
		if (!m->ok) {
			bf3::test::skip_install_case(m->reader.describe().c_str());
			return;
		}
		const level::CollisionSoup &soup = m->soup;
		REQUIRE(soup.tiles.size() == 170);
		CHECK(soup.bodies[0] == 969031);
		CHECK(soup.bodies[1] == 142908);
		CHECK(soup.bodies[2] == 2232);

		const auto load = [&](std::shared_ptr<const StaticWorldShapes> &r_out, level::SoupProgress &r_progress, std::atomic<size_t> &r_done) {
			core::Cache cache;
			std::string detail;
			REQUIRE_MESSAGE(cache.open(cache_options(*m), &detail) == core::CacheStatus::Ok, detail);
			StaticWorldLoadOptions options;
			options.soup_progress = &r_progress;
			options.build.tiles_done = &r_done;
			core::Result<std::shared_ptr<const StaticWorldShapes>> w = load_static_world_cached(&cache, *m->level, *m->world, options);
			REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
			r_out = *w;
			const core::CacheStats stats = cache.stats();
			cache.close();
			return stats;
		};
		level::SoupProgress cold_progress;
		std::atomic<size_t> cold_done{ 0 };
		const core::CacheStats cold_cache = load(m->cold, cold_progress, cold_done);
		const StaticWorldStats &c = m->cold->stats();
		CHECK_FALSE(c.world_hit);
		CHECK(c.tiles_cooked == 170);
		CHECK(c.tiles_hit == 0);
		CHECK(c.seconds_soup > 0.0);
		CHECK(cold_progress.stage.load() == level::SoupProgress::Done);
		CHECK(cold_done.load() == 170);
		CHECK(cold_cache.entries == 171); // 170 tiles + the world index

		level::SoupProgress warm_progress;
		std::atomic<size_t> warm_done{ 0 };
		const core::CacheStats warm_cache = load(m->warm, warm_progress, warm_done);
		const StaticWorldStats &w = m->warm->stats();
		CHECK(w.world_hit);
		CHECK(w.tiles_hit == 170);
		CHECK(w.tiles_cooked == 0);
		CHECK(w.seconds_soup == 0.0);
		CHECK(warm_progress.stage.load() == level::SoupProgress::Mount); // build_collision_soup never ran
		CHECK(warm_done.load() == 170);
		CHECK(warm_cache.puts == 0);
		CHECK(warm_cache.hits == 171);

		// The soup's tiles, counts and totals, in both.
		for (const StaticWorldShapes *shapes : { m->cold.get(), m->warm.get() }) {
			REQUIRE(shapes->tile_count() == soup.tiles.size());
			size_t count_mismatch = 0;
			for (size_t i = 0; i < soup.tiles.size(); ++i) {
				const StaticTileInfo &t = shapes->tile(i);
				std::array<uint32_t, kStaticLayerCount> bodies{};
				for (const level::SoupTriangle &s : soup.tiles[i].triangles) {
					++bodies[s.body];
				}
				count_mismatch += (t.x != soup.tiles[i].x || t.z != soup.tiles[i].z || t.triangles != bodies) ? 1 : 0;
			}
			CHECK(count_mismatch == 0);
			CHECK(shapes->stats().triangles[0] == 969031);
			CHECK(shapes->stats().triangles[1] == 142908);
			CHECK(shapes->stats().triangles[2] == 2232);
		}
		// Cold and warm: the same blobs and the same answers.
		size_t blob_mismatch = 0;
		for (size_t i = 0; i < m->cold->tile_count(); ++i) {
			blob_mismatch += encode_static_tile_blob(*m->cold, i) == encode_static_tile_blob(*m->warm, i) ? 0 : 1;
		}
		CHECK(blob_mismatch == 0);
		const auto cold_world = world_of(m->cold);
		const auto warm_world = world_of(m->warm);
		// 1,000 seeded rays: from 1-20 m around a random soup triangle's centroid through it and 50 % past it.
		std::mt19937 rng(1015);
		std::uniform_real_distribution<float> dir(-1.0f, 1.0f), dist(1.0f, 20.0f);
		size_t hits = 0, differ = 0;
		for (int i = 0; i < 1000; ++i) {
			const level::SoupTile &tile = soup.tiles[rng() % soup.tiles.size()];
			const level::SoupTriangle &s = tile.triangles[rng() % tile.triangles.size()];
			Vec3 target{};
			for (size_t k = 0; k < 3; ++k) {
				target[k] = (s.v[0][k] + s.v[1][k] + s.v[2][k]) / 3.0f;
			}
			const float d = dist(rng);
			const Vec3 origin{ target[0] + d * dir(rng), target[1] + d * dir(rng), target[2] + d * dir(rng) };
			const Vec3 delta{ 1.5f * (target[0] - origin[0]), 1.5f * (target[1] - origin[1]), 1.5f * (target[2] - origin[2]) };
			const uint32_t mask = (i % 2) == 0 ? query_mask::kMovement : query_mask::kAll;
			StaticHit a, b;
			const bool ha = cold_world->ray(origin, delta, mask, &a);
			const bool hb = warm_world->ray(origin, delta, mask, &b);
			hits += ha ? 1 : 0;
			differ += (ha != hb || (ha && !bits_equal(a, b))) ? 1 : 0;
		}
		CHECK(differ == 0);
		CHECK(hits > 700);

		// Memory figures (docs §5.4).
		const auto bodies = cold_world->impl().bodies.size();
		MESSAGE("Metro static world: ", c.tiles, " tiles, ", c.shapes, " MeshShapes (", bodies, " bodies per CollisionWorld), ", c.tags, " tags; resident ",
				mib(c.shape_bytes), " MiB (Jolt GetStats) for ", soup.triangles, " triangles = ", double(c.shape_bytes) / double(soup.triangles),
				" B/triangle; cache ", mib(cold_cache.bytes), " MiB on disk in ", cold_cache.entries, " entries (", mib(c.cache_bytes), " MiB of blobs); soup ",
				mib(uint64_t(soup.triangles) * sizeof(level::SoupTriangle)), " MiB of triangles (", sizeof(level::SoupTriangle), " B each)");
		MESSAGE("cold: soup ", c.seconds_soup, " s, cook ", c.seconds_cook, " s, total ", c.seconds_total, " s; warm: restore ", w.seconds_restore, " s, total ",
				w.seconds_total, " s; peak working set so far ", mib(peak_working_set()), " MiB (timings noisy: other builds share the machine)");

		// A second mode of the level: the tiles it shares with TDM come from the cache (content keys).
		core::Result<std::unique_ptr<level::LoadedLevel>> cq = level::load_level(m->install, kLevel, "ConquestLarge0");
		REQUIRE(cq.ok());
		core::Result<std::unique_ptr<level::World>> cq_world = level::World::traverse(**cq);
		REQUIRE(cq_world.ok());
		{
			core::Cache cache;
			REQUIRE(cache.open(cache_options(*m)) == core::CacheStatus::Ok);
			core::Result<std::shared_ptr<const StaticWorldShapes>> cq_shapes = load_static_world_cached(&cache, **cq, **cq_world);
			REQUIRE(cq_shapes.ok());
			const StaticWorldStats &s = (*cq_shapes)->stats();
			MESSAGE("MP_Subway ConquestLarge0 after TDM: ", s.tiles, " tiles, ", s.tiles_hit, " from the cache, ", s.tiles_cooked, " cooked");
			CHECK_FALSE(s.world_hit);
			CHECK(s.tiles_hit > 0);
			cache.close();
		}
		*cq_world = nullptr;
	}

	TEST_CASE("sim.world: every source the soup read is in a bundle whose manifest the source digest hashed") {
		Metro *m = metro();
		if (!m->ok) {
			bf3::test::skip_install_case(m->reader.describe().c_str());
			return;
		}
		const core::Result<level::SourceDigest> digest = level::level_source_digest(*m->level);
		REQUIRE(digest.ok());
		REQUIRE(digest->digest.has_value());
		const level::BundleSet &set = m->level->bundles;
		CHECK(digest->bundles == 1 + set.settings.size() + set.subworlds.size());
		const core::Result<level::SourceDigest> again = level::level_source_digest(*m->level);
		REQUIRE(again.ok());
		CHECK(again->digest == digest->digest);
		MESSAGE("source digest ", digest->digest->hex(), ": ", digest->bundles, " bundles, ", digest->entries, " manifest entries, ", digest->seconds * 1000.0, " ms");

		// The entries of the hashed bundles, as the digest reads them.
		const core::LevelMount &mount = m->level->mount();
		std::vector<std::string> names{ set.level };
		names.insert(names.end(), set.settings.begin(), set.settings.end());
		names.insert(names.end(), set.subworlds.begin(), set.subworlds.end());
		std::unordered_set<std::string> ebx, res;
		std::set<core::Guid> chunks;
		for (size_t b = 0; b < mount.bundle_count(); ++b) {
			if (std::find(names.begin(), names.end(), mount.bundle_name(b)) == names.end()) {
				continue;
			}
			core::Result<core::BundleManifest> manifest = mount.read_manifest(b);
			REQUIRE(manifest.ok());
			for (const core::EbxEntry &e : manifest->ebx) {
				ebx.insert(core::asset_key(e.name));
			}
			for (const core::ResEntry &e : manifest->res) {
				res.insert(core::asset_key(e.name));
			}
			for (const core::ChunkEntry &e : manifest->chunks) {
				chunks.insert(e.id);
			}
		}
		std::array<size_t, 3> sources{}, missing{};
		for (const level::SoupSource &s : m->soup.sources) {
			const size_t kind = size_t(s.kind);
			++sources[kind];
			const bool found = s.kind == level::SoupSource::Kind::Ebx ? ebx.count(s.name) != 0
					: s.kind == level::SoupSource::Kind::Res		   ? res.count(s.name) != 0
																	   : chunks.count(s.id) != 0;
			if (!found) {
				if (missing[kind]++ < 5) {
					MESSAGE("outside the digest: kind ", int(kind), " ", s.kind == level::SoupSource::Kind::Chunk ? s.id.to_string() : s.name);
				}
			}
		}
		MESSAGE("soup sources: ", sources[0], " EBX partitions, ", sources[1], " res, ", sources[2], " chunks; outside the digest: ", missing[0], " / ", missing[1],
				" / ", missing[2]);
		CHECK(sources[0] > 100);
		CHECK(sources[1] > 100);
		CHECK(sources[2] > 0);
		CHECK_FALSE(mount.has_chunk(core::Guid{})); // the zero id the soup skips (a streaming node without data)
		CHECK(missing == std::array<size_t, 3>{});
	}

	TEST_CASE("sim.world: CollisionWorld answers on Metro equal a brute-force reference over the soup triangles (sampled)") {
		Metro *m = metro();
		if (!m->ok) {
			bf3::test::skip_install_case(m->reader.describe().c_str());
			return;
		}
		std::shared_ptr<const StaticWorldShapes> shapes = m->cold;
		if (shapes == nullptr) {
			core::Result<std::shared_ptr<const StaticWorldShapes>> built = build_static_world(m->soup);
			REQUIRE(built.ok());
			shapes = *built;
		}
		const auto world = world_of(shapes);
		const Reference ref = make_reference(m->soup);
		REQUIRE(ref.triangles.size() == 1114171);
		std::mt19937 rng(20261015);
		std::uniform_real_distribution<double> unit(0.0, 1.0);
		const auto random_spot = [&](uint32_t p_mask, D3 &r_point, D3 &r_normal) {
			for (;;) {
				const RefTriangle &t = ref.triangles[rng() % ref.triangles.size()];
				if (((p_mask >> t.layer) & 1u) == 0) {
					continue;
				}
				double a = unit(rng), b = unit(rng);
				if (a + b > 1) {
					a = 1 - a;
					b = 1 - b;
				}
				r_point = t.v[0] + (t.v[1] - t.v[0]) * a + (t.v[2] - t.v[0]) * b;
				const D3 n = cross(t.v[1] - t.v[0], t.v[2] - t.v[0]);
				r_normal = n * (1.0 / len(n));
				if ((rng() & 1) != 0) {
					r_normal = r_normal * -1.0; // either side: back faces collide
				}
				return;
			}
		};
		const auto random_dir = [&]() {
			for (;;) {
				const D3 d{ unit(rng) * 2 - 1, unit(rng) * 2 - 1, unit(rng) * 2 - 1 };
				const double l = len(d);
				if (l > 0.1 && l <= 1) {
					return d * (1.0 / l);
				}
			}
		};
		const auto to_vec = [](D3 p_d) { return Vec3{ float(p_d.x), float(p_d.y), float(p_d.z) }; };

		// Rays: aimed at a sampled triangle from 0.2-6 m off its plane, 1,000 for movement and 1,000 for everything.
		constexpr double kRayTolerance = 0.002; // metres along the ray and in the plane
		size_t ray_hits = 0, ray_misses = 0, ray_ambiguous = 0, ray_bad = 0, tag_checked = 0, tag_bad = 0;
		for (int i = 0; i < 2000; ++i) {
			const uint32_t mask = i < 1000 ? query_mask::kMovement : query_mask::kAll;
			D3 p, n;
			random_spot(mask, p, n);
			const D3 origin_d = p + n * (0.2 + 5.8 * unit(rng)) + random_dir() * (2.0 * unit(rng));
			D3 target = p + random_dir() * (3.0 * unit(rng));
			D3 delta_d = (target - origin_d) * (1.0 + 2.0 * unit(rng));
			const Vec3 origin = to_vec(origin_d), delta = to_vec(delta_d);
			// The reference works on the float inputs the world saw.
			const D3 o = d3(origin), dl = d3(delta);
			const double length = len(dl);
			std::vector<RayCandidate> candidates;
			const D3 a = o, b = o + dl;
			for_candidates(ref, mask, D3{ std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) }, D3{ std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) },
					0.01, [&](const RefTriangle &t) {
						if (std::optional<RayCandidate> c = ray_candidate(o, dl, t, kRayTolerance)) {
							candidates.push_back(*c);
						}
					});
			// sure: inside by the tolerance, not grazing, inside the segment; possible: within the tolerance.
			std::optional<double> sure;
			for (const RayCandidate &c : candidates) {
				if (!c.grazing && c.margin >= kRayTolerance && c.t >= 0 && c.t <= 1 && (!sure || c.t < *sure)) {
					sure = c.t;
				}
			}
			StaticHit hit;
			const bool jolt = world->ray(origin, delta, mask, &hit);
			if (!jolt) {
				if (sure) {
					++ray_bad;
					if (ray_bad <= 5) {
						MESSAGE("ray ", i, ": Jolt misses, the reference hits at ", *sure * length, " m");
					}
				} else {
					++ray_misses;
				}
				continue;
			}
			++ray_hits;
			const double tj = double(hit.fraction);
			bool explained = false;
			for (const RayCandidate &c : candidates) {
				explained = explained || (c.margin >= -kRayTolerance && std::fabs(c.t - tj) * length <= kRayTolerance + (c.grazing ? 0.05 : 0.0));
			}
			const bool earlier = sure && (*sure - tj) * length < -kRayTolerance;
			if (!explained || earlier || ((mask >> uint32_t(hit.layer)) & 1u) == 0) {
				++ray_bad;
				if (ray_bad <= 5) {
					MESSAGE("ray ", i, ": Jolt hits at ", tj * length, " m, reference sure hit ", sure ? *sure * length : -1.0, " m, explained ", explained);
				}
				continue;
			}
			// When one triangle alone explains the hit, its tag, layer and tile are the hit's, and the normal is its
			// face normal turned to the ray.
			const RayCandidate *only = nullptr;
			size_t close_by = 0;
			for (const RayCandidate &c : candidates) {
				if (c.margin >= -kRayTolerance && std::fabs(c.t - tj) * length <= 0.05) {
					++close_by;
					only = &c;
				}
			}
			if (close_by == 1 && only->margin >= kRayTolerance && !only->grazing) {
				++tag_checked;
				const D3 fn = cross(only->tri->v[1] - only->tri->v[0], only->tri->v[2] - only->tri->v[0]);
				const double align = std::fabs(dot(fn * (1.0 / len(fn)), d3(hit.normal)));
				const bool good = hit.tag == only->tri->tag && uint32_t(hit.layer) == only->tri->layer && hit.tile == only->tri->tile && align > 0.999 &&
						dot(d3(hit.normal), dl) <= 0;
				if (!good) {
					++tag_bad;
					if (tag_bad <= 5) {
						MESSAGE("ray ", i, ": hit tag/layer/tile/normal differ from the only reference triangle (align ", align, ")");
					}
				}
			} else {
				++ray_ambiguous;
			}
		}
		MESSAGE("rays: ", ray_hits, " hits, ", ray_misses, " misses, ", ray_bad, " disagree; tags checked on ", tag_checked, " unique hits, ", tag_bad, " differ, ",
				ray_ambiguous, " hits with several triangles in reach");
		CHECK(ray_bad == 0);
		CHECK(tag_bad == 0);
		CHECK(ray_hits > 1500);
		CHECK(tag_checked > 1000);

		// Capsule overlaps (stand 0.35 x 1.8 m) with their feet 0.6 m around sampled surfaces.
		constexpr double kOverlapTolerance = 0.002;
		size_t overlap_yes = 0, overlap_no = 0, overlap_ambiguous = 0, overlap_bad = 0;
		for (int i = 0; i < 1000; ++i) {
			D3 p, n;
			random_spot(query_mask::kMovement, p, n);
			const Vec3 feet = to_vec(p + n * (1.6 * unit(rng) - 0.2) + random_dir() * (0.8 * unit(rng)) - D3{ 0, 0.9 * unit(rng), 0 });
			const double r = 0.35, h = 1.8;
			const auto axis = capsule_axis(d3(feet), r, h);
			std::vector<const RefTriangle *> close_by;
			for_candidates(ref, query_mask::kMovement, d3(feet) - D3{ r, 0, r }, d3(feet) + D3{ r, h, r }, 0.01, [&](const RefTriangle &t) { close_by.push_back(&t); });
			const double d = capsule_distance(close_by, axis, D3{});
			const bool jolt = world->overlap_capsule(feet, float(r), float(h), query_mask::kMovement);
			if (d < r - kOverlapTolerance) {
				overlap_bad += jolt ? 0 : 1;
				++overlap_yes;
			} else if (d > r + kOverlapTolerance) {
				overlap_bad += jolt ? 1 : 0;
				++overlap_no;
			} else {
				++overlap_ambiguous;
			}
		}
		MESSAGE("capsule overlaps: ", overlap_yes, " overlapping, ", overlap_no, " clear, ", overlap_ambiguous, " within 2 mm of touching, ", overlap_bad, " disagree");
		CHECK(overlap_bad == 0);
		CHECK(overlap_yes > 200);
		CHECK(overlap_no > 100);
		CHECK(overlap_ambiguous < 20);

		// Prone footprint boxes (0.3 x 0.175 x 1.2 m half extents, any yaw). Jolt's box has a 0.05 m convex radius, so
		// its corners are rounded by up to 0.05 (sqrt 3 - 1) = 0.037 m: closer than 0.04 m is ambiguous.
		constexpr double kBoxTolerance = 0.04;
		size_t box_yes = 0, box_no = 0, box_ambiguous = 0, box_bad = 0;
		for (int i = 0; i < 500; ++i) {
			D3 p, n;
			random_spot(query_mask::kMovement, p, n);
			const D3 center = p + n * (1.2 * unit(rng) - 0.1) + random_dir() * (0.8 * unit(rng));
			const D3 half{ 0.3, 0.175, 1.2 };
			const double yaw = unit(rng) * 6.283185307179586;
			std::vector<const RefTriangle *> close_by;
			for_candidates(ref, query_mask::kMovement, center - D3{ 1.3, 1.3, 1.3 }, center + D3{ 1.3, 1.3, 1.3 }, 0.0, [&](const RefTriangle &t) { close_by.push_back(&t); });
			double separation = 1e30;
			const Vec3 c = to_vec(center);
			for (const RefTriangle *t : close_by) {
				separation = std::min(separation, box_triangle_separation(d3(c), half, double(float(yaw)), *t));
			}
			const bool jolt = world->overlap_box(c, to_vec(half), float(yaw), query_mask::kMovement);
			if (separation < -kBoxTolerance) {
				box_bad += jolt ? 0 : 1;
				++box_yes;
			} else if (separation > kBoxTolerance) {
				box_bad += jolt ? 1 : 0;
				++box_no;
			} else {
				++box_ambiguous;
			}
		}
		MESSAGE("box overlaps: ", box_yes, " overlapping, ", box_no, " clear, ", box_ambiguous, " within 4 cm of touching, ", box_bad, " disagree");
		CHECK(box_bad == 0);
		CHECK(box_yes > 50);
		CHECK(box_no > 80);

		// Capsule sweeps (stand 1.8 m and crouch 1.15 m, r 0.35) of 0.5-4 m, mostly towards a sampled surface, from
		// a start the reference finds clear by 5 cm (starts in contact are the overlap rows above). Jolt's fraction
		// must lie between the first contact of a capsule 3 mm thinner and of one 3 mm fatter (both by conservative
		// advancement over the soup triangles).
		constexpr double kSweepTolerance = 0.003;
		size_t sweep_hits = 0, sweep_misses = 0, sweep_bad = 0, sweep_skipped = 0;
		for (int i = 0; i < 400; ++i) {
			const double r = 0.35, h = (i % 3 == 0) ? 1.15 : 1.8;
			Vec3 feet{};
			std::vector<const RefTriangle *> close_by;
			std::pair<D3, D3> axis;
			Vec3 delta{};
			bool found = false;
			for (int attempt = 0; attempt < 40 && !found; ++attempt) {
				D3 p, n;
				random_spot(query_mask::kMovement, p, n);
				const D3 start = p + n * (0.45 + 1.5 * unit(rng)) + D3{ 0, n.y < -0.5 ? -h : (std::fabs(n.y) < 0.5 ? -0.5 * h : 0.0), 0 };
				D3 direction = (p - start) * (1.0 / std::max(1e-6, len(p - start))) + random_dir() * 0.6;
				direction = direction * (1.0 / len(direction));
				feet = to_vec(start);
				delta = to_vec(direction * (0.5 + 3.5 * unit(rng)));
				axis = capsule_axis(d3(feet), r, h);
				close_by.clear();
				const D3 a = d3(feet), b = d3(feet) + d3(delta);
				for_candidates(ref, query_mask::kMovement, D3{ std::min(a.x, b.x) - r, std::min(a.y, b.y), std::min(a.z, b.z) - r },
						D3{ std::max(a.x, b.x) + r, std::max(a.y, b.y) + h, std::max(a.z, b.z) + r }, 0.01, [&](const RefTriangle &t) { close_by.push_back(&t); });
				found = capsule_distance(close_by, axis, D3{}) > r + 0.05;
			}
			if (!found) {
				++sweep_skipped;
				continue;
			}
			const D3 dl = d3(delta);
			const std::optional<double> thin = first_contact(close_by, axis, dl, r - kSweepTolerance);
			const std::optional<double> fat = first_contact(close_by, axis, dl, r + kSweepTolerance);
			StaticHit hit;
			const bool jolt = world->sweep_capsule(feet, float(r), float(h), delta, query_mask::kMovement, &hit);
			bool good;
			if (!jolt) {
				good = !thin.has_value();
				++sweep_misses;
			} else {
				const double tj = double(hit.fraction);
				good = fat.has_value() && *fat <= tj + 1e-4 && (!thin.has_value() || *thin >= tj - 1e-4);
				++sweep_hits;
			}
			if (!good) {
				++sweep_bad;
				if (sweep_bad <= 5) {
					MESSAGE("sweep ", i, ": Jolt ", jolt ? double(hit.fraction) : -1.0, ", thin ", thin ? *thin : -1.0, ", fat ", fat ? *fat : -1.0, " (length ", len(dl), " m)");
				}
			}
		}
		MESSAGE("capsule sweeps: ", sweep_hits, " hits, ", sweep_misses, " clear, ", sweep_bad, " outside the 3 mm band, ", sweep_skipped, " without a clear start");
		CHECK(sweep_bad == 0);
		CHECK(sweep_hits > 200);
		CHECK(sweep_skipped < 20);
	}

	TEST_CASE("sim.world: install.unchanged") {
		Metro *m = metro();
		if (!m->ok) {
			bf3::test::skip_install_case(m->reader.describe().c_str());
			return;
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m->before, m->reader.snapshot());
		MESSAGE("install.unchanged: ", diff.clean() ? std::to_string(m->before.files.size()) + " files: sizes and mtimes equal" : diff.summary());
		CHECK(m->before.valid);
		CHECK(diff.clean());
	}
}
