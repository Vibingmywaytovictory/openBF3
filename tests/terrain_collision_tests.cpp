#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/core/level/world.h"
#include "bf3/core/thread_pool.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim_harness/nav_input_check.h"
#include "terrain_collision_support.h"
#include "test_support.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace terrain_test;
using bf3::sim::CollisionWorld;
using bf3::sim::StaticWorldShapes;

namespace {

namespace fs = std::filesystem;
namespace sim = bf3::sim;

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

std::shared_ptr<const StaticWorldShapes> terrain_world(const level::SoupTerrain &p_record, std::string *r_error = nullptr) {
	bf3::core::Result<sim::StaticTerrainView> view = sim::static_terrain_view(p_record);
	if (!view) {
		if (r_error != nullptr) {
			*r_error = view.error().message;
		}
		return nullptr;
	}
	bf3::core::Result<std::shared_ptr<const StaticWorldShapes>> w = sim::build_static_world_terrain({}, std::span<const sim::StaticTerrainView>(&*view, 1));
	if (!w) {
		if (r_error != nullptr) {
			*r_error = w.error().message;
		}
		return nullptr;
	}
	return *w;
}

std::unique_ptr<CollisionWorld> collision_world(std::shared_ptr<const StaticWorldShapes> p_shapes) {
	bf3::core::Result<std::unique_ptr<CollisionWorld>> w = CollisionWorld::create(std::move(p_shapes));
	REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
	return std::move(*w);
}

RayCheck cast_cells(const CollisionWorld &p_world, const level::SoupTerrain &p_record, uint32_t p_placement, size_t p_step, double p_tol) {
	RayCheck r;
	const res::HeightGrid &g = *p_record.grid;
	if (g.width < 2 || g.height < 2) {
		return r;
	}
	float lo = 0.0f, hi = 0.0f;
	bool first = true;
	for (size_t i = 0; i < g.heights.size(); ++i) {
		if (std::isfinite(g.heights[i])) {
			lo = first ? g.heights[i] : std::min(lo, g.heights[i]);
			hi = first ? g.heights[i] : std::max(hi, g.heights[i]);
			first = false;
		}
	}

	const sim::StaticWorldShapes &s = *p_world.shapes();
	const double lo_y = double(std::min(lo, s.min()[1])) - 5.0, hi_y = double(std::max(hi, s.max()[1])) + 5.0;
	const size_t cw = g.width - 1, ch = g.height - 1;
	for (size_t z = 0; z < ch; z += p_step) {
		for (size_t x = 0; x < cw; x += p_step) {
			for (int half = 0; half < 2; ++half) {
				cast_cell(p_world, p_record, p_placement, x, z, half, lo_y, hi_y, p_tol, r);
			}
		}
	}
	if (p_step > 1) {
		for (const std::array<size_t, 2> &c : mismatch_cells(g)) {
			for (int half = 0; half < 2; ++half) {
				cast_cell(p_world, p_record, p_placement, c[0], c[1], half, lo_y, hi_y, p_tol, r);
			}
		}
	}
	return r;
}

level::CollisionSoup soup_of(level::SoupTerrain p_record) {
	level::CollisionSoup s;
	s.terrains.push_back(std::move(p_record));
	return s;
}

}

TEST_SUITE("sim-terrain-unit") {
	TEST_CASE("sim.terrain.unit: every hole pattern of a 3 x 3 block: field + patch = the soup's cells, one to one, and rays agree") {
		size_t patterns = 0, with_patch = 0, without_field = 0, empty = 0;
		double worst = 0.0;
		for (uint32_t pattern = 0; pattern < 512; ++pattern) {
			res::HeightGrid g = make_grid(3, 3, { 10.0f, -6.0f }, { 2.0f, 2.0f }, [](size_t x, size_t z) { return 50.0f + 0.3f * float(x) - 0.2f * float(z) + 0.1f * float(x * z); });
			for (size_t i = 0; i < 9; ++i) {
				g.holes[i] = uint8_t((pattern >> i) & 1u);
			}
			const level::SoupTerrain record = make_record(std::move(g));
			std::string error;
			std::shared_ptr<const StaticWorldShapes> shapes = terrain_world(record, &error);
			REQUIRE_MESSAGE(shapes != nullptr, "pattern ", pattern, ": ", error);
			REQUIRE(shapes->terrain_count() == 1);
			const FieldCheck fc = exhaustive_check(*shapes, 0, record, 0);
			CHECK_MESSAGE(fc.ok(), "pattern ", pattern, ": missing ", fc.missing, ", extra ", fc.extra, ", duplicate ", fc.duplicate);
			CHECK(fc.field_triangles + fc.patch_triangles == fc.soup_triangles);
			CHECK(shapes->terrain(0).patch_triangles == fc.patch_triangles);
			worst = std::max(worst, fc.worst);
			with_patch += fc.patch_triangles > 0 ? 1 : 0;
			without_field += shapes->impl().terrains[0].field == nullptr ? 1 : 0;
			empty += fc.soup_triangles == 0 ? 1 : 0;
			std::unique_ptr<CollisionWorld> world = collision_world(shapes);
			const RayCheck rc = cast_cells(*world, record, 0, 1, 0.01);
			CHECK_MESSAGE(rc.missed == 0, "pattern ", pattern);
			CHECK_MESSAGE(rc.open_hits == 0, "pattern ", pattern);
			++patterns;
		}
		MESSAGE("512 patterns: " << with_patch << " with a patch, " << without_field << " without a field body, " << empty << " without triangles; worst corner "
							   << worst << " m");
		CHECK(patterns == 512);
		CHECK(with_patch > 0);
		CHECK(worst < 0.01);
	}

	TEST_CASE("sim.terrain.unit: random grids with holes, slopes and odd sizes match the soup one to one") {
		std::mt19937 rng(20261016u);
		for (int round = 0; round < 12; ++round) {
			const size_t w = 2 + rng() % 70, h = 2 + rng() % 70;
			const float hole_rate = float(rng() % 40) / 100.0f;
			std::uniform_real_distribution<float> bump(-3.0f, 3.0f);
			res::HeightGrid g = make_grid(w, h, { -500.0f + float(rng() % 1000) * 0.37f, 300.0f - float(rng() % 1000) * 0.21f },
					{ 0.5f + float(rng() % 8) * 0.5f, 0.5f + float(rng() % 8) * 0.5f },
					[&](size_t x, size_t z) { return 100.0f + 0.4f * float(x) - 0.25f * float(z) + bump(rng); });
			std::uniform_real_distribution<float> u01(0.0f, 1.0f);
			for (uint8_t &hole : g.holes) {
				hole = u01(rng) < hole_rate ? 1 : 0;
			}
			const level::SoupTerrain record = make_record(std::move(g));
			std::string error;
			std::shared_ptr<const StaticWorldShapes> shapes = terrain_world(record, &error);
			REQUIRE_MESSAGE(shapes != nullptr, error);
			const FieldCheck fc = exhaustive_check(*shapes, 0, record, 0);
			CHECK_MESSAGE(fc.ok(), "round ", round, " (", w, " x ", h, "): missing ", fc.missing, ", extra ", fc.extra, ", duplicate ", fc.duplicate);
			CHECK(fc.worst <= double(shapes->terrain(0).max_error) + 1e-3);
			std::unique_ptr<CollisionWorld> world = collision_world(shapes);
			const RayCheck rc = cast_cells(*world, record, 0, 1, double(shapes->terrain(0).max_error) + 0.01);
			CHECK_MESSAGE(rc.missed == 0, "round ", round);
			CHECK_MESSAGE(rc.open_hits == 0, "round ", round);

			const FieldCheck fb = exhaustive_check_banded(*shapes, 0, record, 0, 3, 4);
			CHECK_MESSAGE((fb.cells == fc.cells && fb.soup_triangles == fc.soup_triangles && fb.field_triangles == fc.field_triangles &&
								  fb.patch_triangles == fc.patch_triangles && fb.missing == fc.missing && fb.extra == fc.extra && fb.duplicate == fc.duplicate &&
								  fb.worst == fc.worst),
					"round ", round, ": banded exhaustive check differs");
			const RayCheck rb = cast_cells_banded(shapes, record, 0, 1, double(shapes->terrain(0).max_error) + 0.01, 3);
			CHECK_MESSAGE((rb.rays == rc.rays && rb.missed == rc.missed && rb.open_rays == rc.open_rays && rb.open_hits == rc.open_hits &&
								  rb.occluded == rc.occluded && rb.worst == rc.worst),
					"round ", round, ": banded lattice differs");
		}
	}

	TEST_CASE("sim.terrain.unit: frames: identity, rotated, scaled and translated are cooked; a mirror, a shear and a non-uniform scale are refused") {
		const auto grid = [] {
			res::HeightGrid g = make_grid(17, 13, { -16.0f, -12.0f }, { 2.0f, 2.0f }, [](size_t x, size_t z) { return 20.0f + std::sin(float(x) * 0.4f) * 2.0f + float(z) * 0.1f; });
			g.holes[5 + 6 * 17] = 1;
			g.holes[9 + 2 * 17] = 1;
			return g;
		};
		const float c = std::cos(0.5f), s = std::sin(0.5f);
		std::vector<std::pair<const char *, res::Transform>> good;
		good.emplace_back("identity", res::Transform::identity());
		res::Transform yaw;
		yaw.axis = { { { c, 0.0f, -s }, { 0.0f, 1.0f, 0.0f }, { s, 0.0f, c } } };
		yaw.origin = { 120.0f, -4.0f, 33.0f };
		good.emplace_back("yaw + translation", yaw);
		res::Transform scaled;
		scaled.axis = { { { 2.0f, 0.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }, { 0.0f, 0.0f, 2.0f } } };
		good.emplace_back("uniform scale 2", scaled);
		for (const auto &[name, frame] : good) {
			const level::SoupTerrain record = make_record(grid(), frame);
			std::string error;
			std::shared_ptr<const StaticWorldShapes> shapes = terrain_world(record, &error);
			REQUIRE_MESSAGE(shapes != nullptr, name, ": ", error);
			const FieldCheck fc = exhaustive_check(*shapes, 0, record, 0);
			CHECK_MESSAGE(fc.ok(), name, ": missing ", fc.missing, ", extra ", fc.extra);
			CHECK_MESSAGE(fc.worst < 0.01, name);
			std::unique_ptr<CollisionWorld> world = collision_world(shapes);
			const RayCheck rc = cast_cells(*world, record, 0, 1, 0.02);
			CHECK_MESSAGE(rc.missed == 0, name);
			CHECK_MESSAGE(rc.open_hits == 0, name);
		}
		std::vector<std::pair<const char *, res::Transform>> bad;
		res::Transform mirror;
		mirror.axis = { { { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
		bad.emplace_back("mirror", mirror);
		res::Transform shear;
		shear.axis = { { { 1.0f, 0.0f, 0.0f }, { 0.3f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
		bad.emplace_back("shear", shear);
		res::Transform stretch;
		stretch.axis = { { { 1.0f, 0.0f, 0.0f }, { 0.0f, 3.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } } };
		bad.emplace_back("non-uniform scale", stretch);
		for (const auto &[name, frame] : bad) {
			const level::SoupTerrain record = make_record(grid(), frame);
			std::string error;
			CHECK_MESSAGE(terrain_world(record, &error) == nullptr, name);
			CHECK_FALSE(error.empty());
		}
	}

	TEST_CASE("sim.terrain.unit: append_soup_terrain_tile over every tile equals hash_soup_terrain's tiles; the hash is the same on 1 and N lanes") {
		for (const std::array<float, 2> origin : { std::array<float, 2>{ -128.0f, -64.0f }, std::array<float, 2>{ -101.3f, 77.7f } }) {
			res::HeightGrid g = make_grid(129, 97, origin, { 2.0f, 2.0f }, [](size_t x, size_t z) { return 5.0f + float((x * 7 + z * 3) % 11) * 0.1f; });
			for (size_t i = 0; i < g.holes.size(); i += 37) {
				g.holes[i] = 1;
			}
			const level::SoupTerrain record = make_record(std::move(g));
			level::WorkBudget b1(size_t(1) << 24), bn(size_t(1) << 24);
			bf3::core::Result<level::SoupTerrainHash> one = level::hash_soup_terrain(record, 7, b1, 1);
			bf3::core::Result<level::SoupTerrainHash> many = level::hash_soup_terrain(record, 7, bn, 8);
			REQUIRE(one.ok());
			REQUIRE(many.ok());
			CHECK(one->hash == many->hash);
			CHECK(one->triangles == many->triangles);
			REQUIRE(one->tiles.size() == many->tiles.size());
			size_t total = 0;
			for (size_t i = 0; i < one->tiles.size(); ++i) {
				CHECK(one->tiles[i].tile == many->tiles[i].tile);
				CHECK(one->tiles[i].sum == many->tiles[i].sum);
				std::vector<level::SoupTriangle> tris;
				level::WorkBudget budget(size_t(1) << 20);
				bf3::core::Result<size_t> added = level::append_soup_terrain_tile(record, 7, one->tiles[i].tile, tris, budget);
				REQUIRE(added.ok());
				CHECK(*added == one->tiles[i].triangles);
				uint64_t sum = 0;
				for (const level::SoupTriangle &t : tris) {
					std::array<int64_t, 9> q{};
					REQUIRE(level::soup_quantise(t.v, q));
					CHECK(level::soup_tile_of(q) == one->tiles[i].tile);
					uint64_t h = level::kFnvOffset;
					level::soup_hash_bytes(h, q, t.material, t.surface, t.body, t.flags);
					sum += h;
				}
				CHECK(sum == one->tiles[i].sum);
				total += tris.size();
			}
			CHECK(total == one->triangles);

			std::vector<level::SoupTriangle> none;
			level::WorkBudget budget(size_t(1) << 20);
			bf3::core::Result<size_t> distant = level::append_soup_terrain_tile(record, 7, { 900, -900 }, none, budget);
			REQUIRE(distant.ok());
			CHECK(*distant == 0);
		}
	}

	TEST_CASE("sim.terrain.unit: the OBF3JTER blob round trips; hostile blobs fail without a crash") {
		res::HeightGrid g = make_grid(33, 21, { 0.0f, 0.0f }, { 2.0f, 2.0f }, [](size_t x, size_t z) { return float(x) * 0.3f + float(z) * 0.2f; });
		g.holes[4 + 3 * 33] = 1;
		g.holes[10 + 10 * 33] = 1;
		const level::SoupTerrain record = make_record(std::move(g));
		std::shared_ptr<const StaticWorldShapes> shapes = terrain_world(record);
		REQUIRE(shapes != nullptr);
		const std::vector<uint8_t> blob = sim::encode_static_terrain_blob(*shapes, 0);
		REQUIRE_FALSE(blob.empty());
		CHECK(sim::encode_static_terrain_blob(*shapes, 1).empty());
		bf3::core::Result<std::shared_ptr<const StaticWorldShapes>> back = sim::decode_static_terrain_blob(blob);
		REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
		REQUIRE((*back)->terrain_count() == 1);
		const sim::StaticTerrainInfo &a = shapes->terrain(0), &b = (*back)->terrain(0);
		CHECK(a.width == b.width);
		CHECK(a.holed == b.holed);
		CHECK(a.patch_triangles == b.patch_triangles);
		CHECK(a.content == b.content);
		CHECK(std::memcmp(&a.min, &b.min, sizeof(a.min)) == 0);
		CHECK(sim::encode_static_terrain_blob(**back, 0) == blob);
		const FieldCheck fc = exhaustive_check(**back, 0, record, 0);
		CHECK(fc.ok());

		size_t truncated_ok = 0;
		for (size_t n = 0; n < blob.size(); n += (n < 256 ? 1 : 97)) {
			truncated_ok += sim::decode_static_terrain_blob(std::span<const uint8_t>(blob.data(), n)).ok() ? 1 : 0;
		}
		CHECK(truncated_ok == 0);
		std::vector<uint8_t> longer = blob;
		longer.push_back(0);
		CHECK_FALSE(sim::decode_static_terrain_blob(longer).ok());

		std::mt19937 rng(77u);
		size_t refused = 0, accepted = 0;
		for (size_t i = 0; i < std::min<size_t>(blob.size(), 400); ++i) {
			for (const uint8_t x : { uint8_t(0x01), uint8_t(0x80), uint8_t(0xFF) }) {
				std::vector<uint8_t> m = blob;
				m[i] ^= x;
				bf3::core::Result<std::shared_ptr<const StaticWorldShapes>> r = sim::decode_static_terrain_blob(m);
				(r.ok() ? accepted : refused) += 1;
				if (r.ok()) {

					std::unique_ptr<CollisionWorld> w = collision_world(*r);
					sim::StaticHit hit;
					(void)w->ray({ 10.0f, 100.0f, 10.0f }, { 0.0f, -200.0f, 0.0f }, sim::query_mask::kAll, &hit);
				}
			}
		}
		for (int round = 0; round < 300; ++round) {
			std::vector<uint8_t> m = blob;
			const int flips = 1 + int(rng() % 8);
			for (int f = 0; f < flips; ++f) {
				m[rng() % m.size()] ^= uint8_t(1u << (rng() % 8));
			}
			bf3::core::Result<std::shared_ptr<const StaticWorldShapes>> r = sim::decode_static_terrain_blob(m);
			(r.ok() ? accepted : refused) += 1;
		}
		MESSAGE("mutated blobs: " << refused << " refused, " << accepted << " accepted (height or edge bytes)");
		CHECK(refused > 0);
	}

	TEST_CASE("sim.terrain.unit: nav_terrain_wet: under a plane by more than a soldier, inside its polygon") {
		level::WaterPlane lake;
		lake.y = 50.0f;
		lake.polygon = { { 0.0f, 0.0f }, { 100.0f, 0.0f }, { 100.0f, 100.0f }, { 0.0f, 100.0f } };
		const std::vector<level::WaterPlane> water{ lake };
		const auto tri = [](float y, float x = 10.0f) {
			return std::array<sim::Vec3, 3>{ sim::Vec3{ x, y, 10.0f }, sim::Vec3{ x + 2.0f, y, 10.0f }, sim::Vec3{ x, y, 12.0f } };
		};
		CHECK(sim::nav_terrain_wet(tri(40.0f), water));
		CHECK(sim::nav_terrain_wet(tri(48.1f), water));
		CHECK_FALSE(sim::nav_terrain_wet(tri(48.3f), water));
		CHECK_FALSE(sim::nav_terrain_wet(tri(60.0f), water));
		CHECK_FALSE(sim::nav_terrain_wet(tri(40.0f, 200.0f), water));
		CHECK_FALSE(sim::nav_terrain_wet(tri(40.0f), {}));

		std::array<sim::Vec3, 3> steep = tri(40.0f);
		steep[2][1] = 49.0f;
		CHECK_FALSE(sim::nav_terrain_wet(steep, water));
	}

	TEST_CASE("sim.terrain.unit: a terrain hit: kNoStaticTile, the terrain index, the terrain tag, the Collision layer, an upward normal") {
		res::HeightGrid g = make_grid(9, 9, { 0.0f, 0.0f }, { 2.0f, 2.0f }, [](size_t, size_t) { return 7.0f; });
		g.holes[2] = 1;
		const level::SoupTerrain record = make_record(std::move(g));
		std::shared_ptr<const StaticWorldShapes> shapes = terrain_world(record);
		REQUIRE(shapes != nullptr);
		std::unique_ptr<CollisionWorld> world = collision_world(shapes);
		REQUIRE(shapes->terrain(0).patch_triangles > 0);
		for (const sim::Vec3 at : { sim::Vec3{ 9.1f, 20.0f, 9.3f }, sim::Vec3{ 1.2f, 20.0f, 0.6f } }) {
			sim::StaticHit hit;
			REQUIRE(world->ray(at, { 0.0f, -30.0f, 0.0f }, sim::query_mask::kAll, &hit));
			CHECK(hit.tile == sim::kNoStaticTile);
			CHECK(hit.terrain == 0);
			CHECK(hit.layer == sim::StaticLayer::Collision);
			CHECK(hit.tag.material == sim::static_terrain_tag().material);
			CHECK(hit.tag.surface == sim::static_terrain_tag().surface);
			CHECK(hit.tag.flags == sim::static_terrain_tag().flags);
			CHECK(hit.position[1] == doctest::Approx(7.0f).epsilon(1e-3));
			CHECK(hit.normal[1] > 0.99f);
			CHECK_FALSE(world->ray(at, { 0.0f, -30.0f, 0.0f }, sim::query_mask::kRaycast, nullptr));
		}
	}

	TEST_CASE("sim.terrain.unit: an all-hole grid and a patch-only grid: no field body, the info entry stays") {
		{
			res::HeightGrid g = make_grid(5, 5, { 0.0f, 0.0f }, { 2.0f, 2.0f }, [](size_t, size_t) { return 3.0f; });
			std::fill(g.holes.begin(), g.holes.end(), uint8_t(1));
			const level::SoupTerrain record = make_record(std::move(g));
			std::shared_ptr<const StaticWorldShapes> shapes = terrain_world(record);
			REQUIRE(shapes != nullptr);
			REQUIRE(shapes->terrain_count() == 1);
			CHECK(shapes->impl().terrains[0].field == nullptr);
			CHECK(shapes->impl().terrains[0].patch == nullptr);
			std::unique_ptr<CollisionWorld> world = collision_world(shapes);
			CHECK_FALSE(world->ray({ 4.0f, 10.0f, 4.0f }, { 0.0f, -20.0f, 0.0f }, sim::query_mask::kAll, nullptr));
		}
		{

			res::HeightGrid g = make_grid(3, 3, { 0.0f, 0.0f }, { 2.0f, 2.0f }, [](size_t x, size_t z) { return 3.0f + 0.1f * float(x + z); });
			g.holes[2] = 1;
			g.holes[6] = 1;
			g.holes[8] = 1;
			const level::SoupTerrain record = make_record(std::move(g));
			std::shared_ptr<const StaticWorldShapes> shapes = terrain_world(record);
			REQUIRE(shapes != nullptr);
			REQUIRE(shapes->terrain_count() == 1);
			const FieldCheck fc = exhaustive_check(*shapes, 0, record, 0);
			CHECK(fc.ok());
			CHECK(fc.soup_triangles == 2);
			CHECK(fc.patch_triangles == 2);
			CHECK(shapes->impl().terrains[0].field == nullptr);
			CHECK(shapes->terrain(0).patch_triangles == 2);
			std::unique_ptr<CollisionWorld> world = collision_world(shapes);
			const RayCheck rc = cast_cells(*world, record, 0, 1, 0.01);
			CHECK(rc.missed == 0);
			CHECK(rc.open_hits == 0);
			CHECK(rc.rays == 2);
		}
	}

	TEST_CASE("sim.terrain.unit: soup_terrain_cell on a null grid and out of range; budgets one unit short fail") {
		level::SoupTerrain none;
		const level::SoupTerrainCell c0 = level::soup_terrain_cell(none, 0, 0, 0);
		CHECK(c0.open);
		CHECK(c0.count == 0);
		const level::SoupTerrain record = make_record(make_grid(4, 3, { 0.0f, 0.0f }, { 1.0f, 1.0f }, [](size_t, size_t) { return 1.0f; }));
		CHECK(level::soup_terrain_cell(record, 0, 2, 1).count == 2);
		CHECK(level::soup_terrain_cell(record, 0, 3, 0).open);
		CHECK(level::soup_terrain_cell(record, 0, 0, 2).open);
		CHECK(level::soup_terrain_cell(record, 0, size_t(-1), 0).open);
		level::WorkBudget short_budget(2 * 3 * 2 - 1);
		CHECK_FALSE(level::hash_soup_terrain(record, 0, short_budget).ok());
		level::WorkBudget exact(2 * 3 * 2);
		CHECK(level::hash_soup_terrain(record, 0, exact).ok());
		std::vector<level::SoupTriangle> out;
		level::WorkBudget empty(0);
		CHECK_FALSE(level::append_soup_terrain_tile(record, 0, { 0, 0 }, out, empty).ok());
		CHECK_FALSE(level::append_soup_terrain_tile(none, 0, { 0, 0 }, out, empty).ok());

		res::HeightGrid broken = make_grid(4, 4, { 0.0f, 0.0f }, { 1.0f, 1.0f }, [](size_t, size_t) { return 1.0f; });
		broken.heights.resize(3);
		const level::SoupTerrain bad = make_record(std::move(broken));
		CHECK(level::soup_terrain_cell(bad, 0, 0, 0).open);
		level::WorkBudget b(1000);
		CHECK_FALSE(level::hash_soup_terrain(bad, 0, b).ok());
		CHECK_FALSE(sim::static_terrain_view(bad).ok());
	}

	TEST_CASE("sim.terrain.unit: a synthetic soup's nav input matches the terrain fields one to one (proof 1), wet cells marked") {
		res::HeightGrid g = make_grid(65, 65, { -64.0f, -64.0f }, { 2.0f, 2.0f }, [](size_t x, size_t z) { return 40.0f + float(x) * 0.2f + float(z % 5) * 0.05f; });
		g.holes[30 + 30 * 65] = 1;
		level::CollisionSoup soup = soup_of(make_record(std::move(g)));
		bf3::core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = sim::build_static_world(soup);
		REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
		REQUIRE((*shapes)->terrain_count() == 1);
		level::WaterPlane lake;
		lake.y = 45.0f;
		lake.polygon = { { -64.0f, -64.0f }, { 0.0f, -64.0f }, { 0.0f, 64.0f }, { -64.0f, 64.0f } };
		const std::vector<level::WaterPlane> water{ lake };
		const std::vector<sim::NavTileKey> tiles{ { -1, -1 }, { -1, 0 }, { 0, -1 }, { 0, 0 } };
		const sim::NavParams params;
		bf3::core::Result<std::vector<sim::NavTileInput>> inputs = sim::nav_tile_inputs(soup, tiles, params, sim::query_mask::kMovement, water);
		REQUIRE_MESSAGE(inputs.ok(), (inputs.ok() ? std::string() : inputs.error().message));
		size_t wet = 0, terrain = 0;
		for (const sim::NavTileInput &in : *inputs) {
			terrain += in.terrain_triangles;
			for (uint8_t u : in.unwalkable) {
				wet += u;
			}
			bf3::core::Result<sim::NavInputCheck> chk = sim::nav_check_input_against_world(**shapes, in, params);
			REQUIRE_MESSAGE(chk.ok(), (chk.ok() ? std::string() : chk.error().message));
			MESSAGE("tile (" << in.tile[0] << ", " << in.tile[1] << "): " << in.terrain_triangles << " terrain input triangles, world " << chk->world_triangles
							 << ", matched " << chk->matched << ", nav_only " << chk->nav_only << ", world_only " << chk->world_only << ", tolerance "
							 << chk->terrain_tolerance << ", max corner error " << chk->max_corner_error);
			CHECK(chk->nav_only == 0);
			CHECK(chk->world_only == 0);
			CHECK(chk->terrain_matched == in.terrain_triangles);
		}
		CHECK(terrain > 0);
		CHECK(wet > 0);
	}
}

namespace {

const char *kLevels[] = { "MP_001", "MP_003", "MP_007", "MP_011", "MP_012", "MP_013", "MP_017", "MP_018", "MP_Subway", "XP1_001", "XP1_002", "XP1_003",
	"XP1_004", "XP2_Factory", "XP2_Office", "XP2_Palace", "XP2_Skybar", "XP3_Alborz", "XP3_Desert", "XP3_Shield", "XP3_Valley", "XP4_FD", "XP4_Parl",
	"XP4_Quake", "XP4_Rubble", "XP5_001", "XP5_002", "XP5_003", "XP5_004" };

uint32_t expected_holed(const std::string &p_level) {
	if (p_level == "MP_001" || p_level == "XP4_Parl") {
		return 1;
	}
	if (p_level == "MP_003") {
		return 2;
	}
	return 0;
}

struct Shared {
	bf3::core::InstallReader reader;
	bf3::core::InstallSnapshot before;
	std::shared_ptr<bf3::core::Install> install;
	bool ok = false;
};

Shared *shared() {
	static Shared s;
	static const bool done = [] {
		if (!s.reader.locate({})) {
			return false;
		}
		s.before = s.reader.snapshot();
		bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(s.reader);
		if (!install) {
			return false;
		}
		s.install = *install;
		s.ok = true;
		return true;
	}();
	(void)done;
	return &s;
}

unsigned env_unsigned(const char *p_name, unsigned p_default) {
	char *value = nullptr;
	size_t size = 0;
	unsigned out = p_default;
	if (_dupenv_s(&value, &size, p_name) == 0 && value != nullptr && value[0] != '\0') {
		out = unsigned(std::strtoul(value, nullptr, 10));
	}
	std::free(value);
	return out;
}

unsigned g1_lanes() {
	const unsigned forced = env_unsigned("OPENBF3_TERRAIN_LANES", 0);
	if (forced != 0) {
		return forced;
	}
	MEMORYSTATUSEX m{};
	m.dwLength = sizeof(m);
	uint64_t avail = 0;
	if (GlobalMemoryStatusEx(&m)) {
		avail = m.ullAvailPhys;
	}
	unsigned lanes = unsigned(std::clamp<uint64_t>(avail / (650ull << 20), 2, 8));
#if defined(__SANITIZE_ADDRESS__)
	lanes = std::min(lanes, 4u);
#endif
	return lanes;
}

struct Scratch {
	fs::path root;
	Scratch() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		root = fs::path(tmp) / (L"o3t-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
	}
	~Scratch() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}
};

struct RunResult {
	std::string mode;
	bool ok = false;
	std::string error;
	std::vector<std::string> digests;
	size_t terrains = 0;
	std::optional<level::SoupTerrain> record;
	uint32_t placement = 0;
	std::vector<std::array<int32_t, 2>> tiles;
	double seconds = 0.0;
};

RunResult run_one(const std::shared_ptr<bf3::core::Install> &p_install, const std::string &p_level, const std::string &p_mode, bool p_keep, unsigned p_threads) {
	RunResult r;
	r.mode = p_mode;
	const auto start = std::chrono::steady_clock::now();
	bf3::core::Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(p_install, p_level, p_mode);
	if (!loaded) {
		r.error = loaded.error().message;
		return r;
	}
	bf3::core::Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
	if (!world) {
		r.error = world.error().message;
		return r;
	}
	level::SoupOptions options;
	options.threads = p_threads;
	bf3::core::Result<level::CollisionSoup> soup = level::build_collision_soup(**loaded, **world, options);
	if (!soup) {
		r.error = soup.error().message;
		return r;
	}
	for (size_t i = 0; i < soup->terrains.size(); ++i) {
		const level::SoupTerrain &t = soup->terrains[i];
		if (t.state != level::SoupTerrain::State::Built) {
			continue;
		}
		bf3::core::Result<sim::StaticTerrainView> view = sim::static_terrain_view(t);
		if (!view) {
			r.error = view.error().message;
			return r;
		}
		r.digests.push_back(sim::static_terrain_content(*view).hex());
		if (r.terrains == 0 && p_keep) {
			r.record = t;
			r.placement = uint32_t(soup->placements.size() + i);
		}
		++r.terrains;
	}
	bf3::core::Result<level::ModeAreas> areas = level::read_mode_areas(**world);
	if (areas) {
		bf3::core::Result<std::vector<std::array<int32_t, 2>>> tiles =
				level::combat_area_tiles(std::span<const level::CombatAreaDef>(areas->combat_areas), sim::NavParams().region_margin);
		if (tiles) {
			r.tiles = std::move(*tiles);
		}
	}
	r.seconds = seconds_since(start);
	r.ok = true;
	return r;
}

bool cell_under(const level::SoupTerrain &p_record, double p_x, double p_z, size_t &r_x, size_t &r_z) {
	double u = 0, v = 0;
	to_grid(p_record, { p_x, 0.0, p_z }, u, v);
	const res::HeightGrid &g = *p_record.grid;
	if (!(u >= 0 && v >= 0 && u < double(g.width - 1) && v < double(g.height - 1))) {
		return false;
	}
	r_x = size_t(u);
	r_z = size_t(v);
	return true;
}

struct TileCentres {
	size_t tiles = 0, outside = 0, open = 0;
	RayCheck rays;
};
TileCentres cast_tile_centres(const CollisionWorld &p_world, const level::SoupTerrain &p_record, uint32_t p_placement,
		const std::vector<std::array<int32_t, 2>> &p_tiles, double p_tol) {
	TileCentres out;
	for (const std::array<int32_t, 2> &t : p_tiles) {
		++out.tiles;
		size_t x = 0, z = 0;
		if (!cell_under(p_record, double(t[0]) * 64.0 + 32.0, double(t[1]) * 64.0 + 32.0, x, z)) {
			++out.outside;
			continue;
		}
		if (level::soup_terrain_cell(p_record, p_placement, x, z).open) {
			++out.open;
		}
		for (int half = 0; half < 2; ++half) {
			cast_cell(p_world, p_record, p_placement, x, z, half, -2000.0, 4000.0, p_tol, out.rays);
		}
	}
	return out;
}

struct LevelCheck {
	bool kept = false;
	bool cooked = false;
	std::string error;
	sim::StaticTerrainInfo ti;
	FieldCheck fc;
	RayCheck rc;
	size_t centres = 0, centres_outside = 0, centres_open = 0, centre_rays = 0, centre_bad = 0;
	double centre_worst = 0.0;
	double cook = 0.0, ex = 0.0, rays = 0.0;
};

LevelCheck check_level(std::span<RunResult> p_results, unsigned p_lanes) {
	LevelCheck out;
	RunResult *kept = nullptr;
	for (RunResult &r : p_results) {
		if (r.ok && r.record) {
			kept = &r;
			break;
		}
	}
	if (kept == nullptr) {
		return out;
	}
	out.kept = true;
	const level::SoupTerrain &record = *kept->record;
	const uint32_t placement = kept->placement;
	const auto cook_start = std::chrono::steady_clock::now();
	std::shared_ptr<const StaticWorldShapes> shapes = terrain_world(record, &out.error);
	if (shapes == nullptr) {
		kept->record.reset();
		return out;
	}
	out.cooked = true;
	out.cook = seconds_since(cook_start);
	out.ti = shapes->terrain(0);
	const auto ex_start = std::chrono::steady_clock::now();
	out.fc = exhaustive_check_banded(*shapes, 0, record, placement, p_lanes);
	out.ex = seconds_since(ex_start);
	std::unique_ptr<CollisionWorld> world = collision_world(shapes);

	const res::HeightGrid &g = *record.grid;
	const size_t step = std::max<size_t>(1, size_t(std::lround(16.0 / std::max(0.01f, g.sample_spacing[0]))));
	const auto ray_start = std::chrono::steady_clock::now();
	out.rc = cast_cells_banded(shapes, record, placement, step, 0.05, p_lanes);

	for (const RunResult &r : p_results) {
		const TileCentres tc = cast_tile_centres(*world, record, placement, r.tiles, 0.05);
		out.centres += tc.tiles;
		out.centres_outside += tc.outside;
		out.centres_open += tc.open;
		out.centre_rays += tc.rays.rays + tc.rays.open_rays;
		out.centre_bad += tc.rays.missed + tc.rays.open_hits;
		out.centre_worst = std::max(out.centre_worst, tc.rays.worst);
	}
	out.rays = seconds_since(ray_start);
	world.reset();
	shapes.reset();
	kept->record.reset();
	return out;
}

}

TEST_SUITE("sim-terrain") {
	TEST_CASE("sim.terrain: G1 every level x mode: one record digest per level, the exhaustive field check, lattice and tile-centre rays") {
		Shared *s = shared();
		if (!s->ok) {
			bf3::test::skip_install_case(s->reader.describe().c_str());
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		const unsigned hw = std::max(1u, std::thread::hardware_concurrency());

		const unsigned lanes = g1_lanes();
		const unsigned soup_threads = 2;
		const unsigned check_lanes = env_unsigned("OPENBF3_TERRAIN_CHECK_LANES", std::max(1u, hw / 2));
		std::printf("sim.terrain G1: %u run lanes x %u soup threads, checks on %u lanes\n", lanes, soup_threads, check_lanes);
		std::fflush(stdout);
		struct Job {
			size_t level;
			std::string mode;
			bool keep;
		};
		std::vector<Job> jobs;
		std::vector<size_t> first_job;
		for (size_t li = 0; li < std::size(kLevels); ++li) {
			const std::string level_name = kLevels[li];
			bf3::core::Result<level::LevelInfo> info = level::level_info(*s->install, level_name);
			REQUIRE_MESSAGE(info.ok(), level_name, ": ", (info.ok() ? std::string() : info.error().message));
			first_job.push_back(jobs.size());
			for (size_t m = 0; m < info->modes.size(); ++m) {
				jobs.push_back({ li, info->modes[m], m == 0 });
			}
		}
		first_job.push_back(jobs.size());
		std::vector<RunResult> all(jobs.size());
		std::vector<LevelCheck> checks(std::size(kLevels));
		std::vector<std::atomic<size_t>> remaining(std::size(kLevels));
		for (size_t li = 0; li < std::size(kLevels); ++li) {
			remaining[li].store(first_job[li + 1] - first_job[li], std::memory_order_relaxed);
		}
		bf3::core::parallel_for(
				jobs.size(),
				[&](size_t i) {
					const size_t li = jobs[i].level;
					all[i] = run_one(s->install, kLevels[li], jobs[i].mode, jobs[i].keep, soup_threads);

					if (remaining[li].fetch_sub(1, std::memory_order_acq_rel) == 1) {
						checks[li] = check_level(std::span<RunResult>(all.data() + first_job[li], first_job[li + 1] - first_job[li]), check_lanes);
					}
				},
				lanes);
		std::printf("sim.terrain G1: %zu runs and their checks in %.1f s\n", all.size(), seconds_since(start));
		std::fflush(stdout);
		size_t runs = 0, runs_without = 0, digests = 0, lattice_rays = 0, lattice_missed = 0, open_rays = 0, open_hits = 0, centre_missed = 0, failures = 0;
		double worst = 0.0;
		for (size_t li = 0; li < std::size(kLevels); ++li) {
			const char *level_id = kLevels[li];
			const std::string level_name = level_id;
			const std::span<const RunResult> results(all.data() + first_job[li], first_job[li + 1] - first_job[li]);
			double level_seconds = 0.0;
			for (const RunResult &r : results) {
				level_seconds += r.seconds;
			}
			runs += results.size();
			std::set<std::string> level_digests;
			size_t with = 0;
			for (const RunResult &r : results) {
				CHECK_MESSAGE(r.ok, level_name, " ", r.mode, ": ", r.error);
				failures += r.ok ? 0 : 1;
				with += r.terrains > 0 ? 1 : 0;
				runs_without += r.terrains == 0 ? 1 : 0;
				for (const std::string &d : r.digests) {
					level_digests.insert(d);
				}
				CHECK_MESSAGE(r.terrains <= 1, level_name, " ", r.mode);
			}

			CHECK_MESSAGE((with == 0 || with == results.size()), level_name, ": ", with, " of ", results.size(), " runs have a terrain");
			CHECK_MESSAGE(level_digests.size() <= 1, level_name, ": ", level_digests.size(), " digests");
			const LevelCheck &lc = checks[li];
			if (level_digests.empty() || !lc.kept) {
				CHECK_MESSAGE(level_digests.empty(), level_name, ": a digest but no kept record (the first mode failed)");
				std::printf("sim.terrain %-12s %zu runs: no terrain (%.1f s)\n", level_id, results.size(), level_seconds);
				continue;
			}
			++digests;
			CHECK_MESSAGE(lc.cooked, level_name, ": ", lc.error);
			if (!lc.cooked) {
				++failures;
				continue;
			}
			const sim::StaticTerrainInfo &ti = lc.ti;
			CHECK(ti.content.hex() == *level_digests.begin());
			CHECK_MESSAGE(ti.holed == expected_holed(level_name), level_name, ": holed ", ti.holed);
			const FieldCheck &fc = lc.fc;
			CHECK_MESSAGE(fc.ok(), level_name, ": missing ", fc.missing, ", extra ", fc.extra, ", duplicate ", fc.duplicate);
			CHECK(fc.field_triangles + fc.patch_triangles == fc.soup_triangles);
			CHECK_MESSAGE(fc.worst <= double(ti.max_error) + 2e-3, level_name, ": worst corner ", fc.worst, " m, max_error ", ti.max_error);
			const RayCheck &rc = lc.rc;
			const size_t centres = lc.centres, centres_outside = lc.centres_outside, centres_open = lc.centres_open, centre_rays = lc.centre_rays,
						 centre_bad = lc.centre_bad;
			worst = std::max(worst, lc.centre_worst);
			const double cook = lc.cook, ex = lc.ex, rays = lc.rays;
			CHECK_MESSAGE(rc.missed == 0, level_name);
			CHECK_MESSAGE(rc.open_hits == 0, level_name);
			CHECK_MESSAGE(centre_bad == 0, level_name);
			lattice_rays += rc.rays;
			lattice_missed += rc.missed;
			open_rays += rc.open_rays;
			open_hits += rc.open_hits;
			centre_missed += centre_bad;
			worst = std::max(worst, rc.worst);
			std::printf("sim.terrain %-12s %zu runs, digest %.16s: %zu x %zu, holes %u, holed %u, patch_triangles %u, max_error %.4f m; exhaustive %zu cells, "
						"%zu soup = %zu field + %zu patch (missing %zu, extra %zu), worst corner %.4f m; lattice %zu rays (%zu missed, worst %.4f m, "
						"%zu occluded), open %zu rays (%zu hits); tile centres %zu (%zu outside the grid, %zu open, %zu rays, %zu bad); cook %.2f s, "
						"exhaustive %.2f s, rays %.2f s, level %.1f s\n",
					level_id, results.size(), ti.content.hex().c_str(), size_t(ti.width), size_t(ti.height), ti.holes, ti.holed, ti.patch_triangles,
					double(ti.max_error), fc.cells, fc.soup_triangles, fc.field_triangles, fc.patch_triangles, fc.missing, fc.extra, fc.worst, rc.rays,
					rc.missed, rc.worst, rc.occluded, rc.open_rays, rc.open_hits, centres, centres_outside, centres_open, centre_rays, centre_bad, cook, ex,
					rays, level_seconds);
			std::fflush(stdout);
		}
		std::printf("sim.terrain G1: %zu runs (%zu without terrain, %zu failed), %zu digests; lattice %zu rays, %zu missed; open-cell %zu rays, %zu hits; "
					"tile centres %zu bad; worst error %.4f m; %.1f s\n",
				runs, runs_without, failures, digests, lattice_rays, lattice_missed, open_rays, open_hits, centre_missed, worst, seconds_since(start));
		CHECK(runs == 230);
		CHECK(runs_without == 31);
		CHECK(digests == 24);
		CHECK(failures == 0);
		CHECK(lattice_missed == 0);
		CHECK(open_hits == 0);
		CHECK(centre_missed == 0);
		CHECK(worst <= 0.05);
	}

	TEST_CASE("sim.terrain: G1 full worlds: Caspian CQL, Kharg CQL and Metro TDM cold then warm through a scratch cache") {
		Shared *s = shared();
		if (!s->ok) {
			bf3::test::skip_install_case(s->reader.describe().c_str());
			return;
		}
		struct Run {
			const char *level;
			const char *mode;
			bool ground_gate;
		};
		for (const Run run : { Run{ "MP_007", "ConquestLarge0", true }, Run{ "MP_018", "ConquestLarge0", true }, Run{ "MP_Subway", "TeamDeathMatch0", false } }) {
			Scratch scratch;
			bf3::core::Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(s->install, run.level, run.mode);
			REQUIRE(loaded.ok());
			bf3::core::Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
			REQUIRE(world.ok());
			bf3::core::Result<level::CollisionSoup> soup = level::build_collision_soup(**loaded, **world);
			REQUIRE(soup.ok());
			REQUIRE(soup->terrains.size() == 1);
			const level::SoupTerrain &record = soup->terrains[0];
			const uint32_t placement = uint32_t(soup->placements.size());
			const auto load = [&](sim::StaticWorldStats &r_stats) {
				bf3::core::Cache cache;
				bf3::core::CacheOptions o;
				o.root = scratch.root;
				o.guard = s->reader.write_guard();
				std::string detail;
				REQUIRE_MESSAGE(cache.open(o, &detail) == bf3::core::CacheStatus::Ok, detail);
				bf3::core::Result<std::shared_ptr<const StaticWorldShapes>> w = sim::load_static_world_cached(&cache, **loaded, **world);
				REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
				r_stats = (*w)->stats();
				cache.close();
				return *w;
			};
			sim::StaticWorldStats cold_stats, warm_stats;
			std::shared_ptr<const StaticWorldShapes> cold = load(cold_stats);
			std::shared_ptr<const StaticWorldShapes> warm = load(warm_stats);
			CHECK_FALSE(cold_stats.world_hit);
			CHECK(cold_stats.terrains == 1);
			CHECK(cold_stats.terrains_cooked == 1);
			CHECK(warm_stats.world_hit);
			CHECK(warm_stats.terrains_hit == 1);
			CHECK(warm_stats.terrains_cooked == 0);
			std::unique_ptr<CollisionWorld> cw = collision_world(cold);
			std::unique_ptr<CollisionWorld> ww = collision_world(warm);
			const res::HeightGrid &g = *record.grid;
			const size_t step = std::max<size_t>(1, size_t(std::lround(16.0 / std::max(0.01f, g.sample_spacing[0]))));
			const RayCheck rc = cast_cells(*cw, record, placement, step, 0.05);
			CHECK_MESSAGE(rc.missed == 0, run.level, ": first miss cell (", rc.miss_x, ", ", rc.miss_z, ") half ", rc.miss_half, ", expected y ", rc.miss_expected,
					", last hit y ", rc.miss_hit_y, ", ", rc.miss_occluders, " occluders");

			CHECK(rc.open_hits == 0);

			bf3::core::Result<level::LevelCombatTiles> combat = level::level_combat_tiles(s->install, run.level, sim::NavParams().region_margin);
			REQUIRE(combat.ok());
			size_t without_ground = 0;
			for (const std::array<int32_t, 2> &t : combat->tiles) {
				const sim::Vec3 top{ float(t[0]) * 64.0f + 32.0f, 3000.0f, float(t[1]) * 64.0f + 32.0f };
				without_ground += cw->ray(top, { 0.0f, -6000.0f, 0.0f }, sim::query_mask::kMovement, nullptr) ? 0 : 1;
			}
			const TileCentres tc = cast_tile_centres(*cw, record, placement, combat->tiles, 0.05);

			std::set<std::pair<int32_t, int32_t>> havok;
			for (const level::SoupTile &t : soup->tiles) {
				havok.insert({ t.x, t.z });
			}
			std::vector<std::array<int32_t, 2>> bare;
			for (const std::array<int32_t, 2> &t : combat->tiles) {
				bool close_by = false;
				for (int32_t dz = -1; dz <= 1 && !close_by; ++dz) {
					for (int32_t dx = -1; dx <= 1 && !close_by; ++dx) {
						close_by = havok.count({ t[0] + dx, t[1] + dz }) != 0;
					}
				}
				if (!close_by) {
					bare.push_back(t);
				}
			}
			if (!bare.empty()) {
				double mx = 0, mz = 0;
				for (const std::array<int32_t, 2> &t : bare) {
					mx += t[0];
					mz += t[1];
				}
				mx /= double(bare.size());
				mz /= double(bare.size());
				const std::array<int32_t, 2> *pick = &bare[0];
				for (const std::array<int32_t, 2> &t : bare) {
					if (std::hypot(t[0] - mx, t[1] - mz) < std::hypot((*pick)[0] - mx, (*pick)[1] - mz)) {
						pick = &t;
					}
				}
				const sim::Vec3 top{ float((*pick)[0]) * 64.0f + 32.0f, 3000.0f, float((*pick)[1]) * 64.0f + 32.0f };
				sim::StaticHit hit;
				const bool ground = cw->ray(top, { 0.0f, -6000.0f, 0.0f }, sim::query_mask::kMovement, &hit);
				CHECK_MESSAGE((ground || !run.ground_gate), run.level);
				std::printf("sim.terrain full %s %s: %zu of %zu combat tiles had no soup triangle before TC; capture tile (%d, %d), centre (%.1f, %.1f, %.1f), "
							"terrain hit %d\n",
						run.level, run.mode, bare.size(), combat->tiles.size(), (*pick)[0], (*pick)[1], double(top[0]), ground ? double(hit.position[1]) : 0.0,
						double(top[2]), ground && hit.terrain != sim::kNoStaticTerrain ? 1 : 0);
			}

			std::mt19937 rng(4242u);
			const sim::Vec3 lo = cold->min(), hi = cold->max();
			std::uniform_real_distribution<float> ux(lo[0], hi[0]), uz(lo[2], hi[2]);
			size_t same = 0, hits = 0;
			for (int i = 0; i < 10000; ++i) {
				const sim::Vec3 o{ ux(rng), hi[1] + 10.0f, uz(rng) };
				const sim::Vec3 d{ 0.0f, lo[1] - hi[1] - 20.0f, 0.0f };
				sim::StaticHit a, b;
				const bool ha = cw->ray(o, d, sim::query_mask::kAll, &a), hb = ww->ray(o, d, sim::query_mask::kAll, &b);
				hits += ha ? 1 : 0;
				same += (ha == hb && (!ha || (std::memcmp(&a.position, &b.position, sizeof(a.position)) == 0 && a.terrain == b.terrain && a.tile == b.tile))) ? 1 : 0;
			}
			std::printf("sim.terrain full %s %s: cold %.2f s (terrain %.2f s, %zu tiles cooked, cache %.1f MiB), warm %.2f s (world hit %d, terrains hit %zu); "
						"lattice %zu rays (%zu missed, %zu occluded by Havok, %zu on Havok ground within 5 cm, worst %.4f m), open %zu rays (%zu terrain hits); combat tiles %zu, %zu without "
						"ground; tile-centre cells %zu rays, %zu bad (%zu outside, %zu open); 10,000 rays cold = warm: %zu (%zu hits)\n",
					run.level, run.mode, cold_stats.seconds_total, cold_stats.seconds_terrain, cold_stats.tiles_cooked, double(cold_stats.cache_bytes) / 1048576.0,
					warm_stats.seconds_total, warm_stats.world_hit ? 1 : 0, warm_stats.terrains_hit, rc.rays, rc.missed, rc.occluded, rc.coincident, rc.worst, rc.open_rays,
					rc.open_hits, combat->tiles.size(), without_ground, tc.rays.rays, tc.rays.missed + tc.rays.open_hits, tc.outside, tc.open, same, hits);
			std::fflush(stdout);
			CHECK(same == 10000);
			CHECK(tc.rays.missed == 0);
			if (run.ground_gate) {
				CHECK_MESSAGE(without_ground == 0, run.level, " ", run.mode);
			}
		}
	}

	TEST_CASE("sim.terrain: G6 Caspian RushLarge0 underground spawns: parked in the data under unholed terrain, nothing to stand on") {
		Shared *s = shared();
		if (!s->ok) {
			bf3::test::skip_install_case(s->reader.describe().c_str());
			return;
		}
		bf3::core::Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(s->install, "MP_007", "RushLarge0");
		REQUIRE(loaded.ok());
		bf3::core::Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
		REQUIRE(world.ok());
		bf3::core::Result<level::LevelSpawns> spawns = level::read_level_spawns(**world);
		REQUIRE(spawns.ok());
		bf3::core::Result<level::CollisionSoup> soup = level::build_collision_soup(**loaded, **world);
		REQUIRE(soup.ok());
		REQUIRE(soup->terrains.size() == 1);
		const level::SoupTerrain &record = soup->terrains[0];
		const bf3::core::EbxResolver &db = (*loaded)->db();
		const auto partition = [&](const level::PlacedObject &p_object) {
			return std::string(db.name_of(p_object.entity.partition().guid()).value_or("?"));
		};

		size_t points = 0, in_logic = 0, direct = 0;
		float y_lo = 1e9f, y_hi = -1e9f, depth_lo = 1e9f, depth_hi = -1e9f;
		size_t open = 0, outside = 0;
		for (const level::SpawnPointDef &p : spawns->points) {
			const float x = p.transform.trans[0], y = p.transform.trans[1], z = p.transform.trans[2];
			if (x < -112.0f || x > -58.0f || z < 343.0f || z > 370.0f || y > 120.0f) {
				continue;
			}
			++points;
			y_lo = std::min(y_lo, y);
			y_hi = std::max(y_hi, y);
			const level::PlacedObject &object = (*world)->objects[p.object];
			in_logic += partition(object) == "levels/mp_007/rush/layer0_rush_logic" ? 1 : 0;
			direct += !object.reference.has_value() ? 1 : 0;

			size_t cx = 0, cz = 0;
			if (!cell_under(record, x, z, cx, cz)) {
				++outside;
				continue;
			}
			if (level::soup_terrain_cell(record, 0, cx, cz).open) {
				++open;
				continue;
			}
			const res::HeightGrid &g = *record.grid;
			const float h = 0.25f * (g.at(cx, cz) + g.at(cx + 1, cz) + g.at(cx, cz + 1) + g.at(cx + 1, cz + 1));
			depth_lo = std::min(depth_lo, h - y);
			depth_hi = std::max(depth_hi, h - y);
		}

		size_t disabled_spawners = 0;
		std::set<int> teams;
		for (const level::CharacterSpawnerDef &sp : spawns->spawners) {
			const level::PlacedObject &object = (*world)->objects[sp.object];
			const float y = object.transform.trans[1];
			if (partition(object) == "levels/mp_007/rush/layer0_rush_logic" && !sp.enabled && y > 105.0f && y < 113.0f) {
				++disabled_spawners;
				teams.insert(sp.team);
			}
		}

		size_t havok = 0;
		for (const level::SoupTile &t : soup->tiles) {
			for (const level::SoupTriangle &tri : t.triangles) {
				for (const res::Vec3 &v : tri.v) {
					if (v[0] >= -112.0f && v[0] <= -58.0f && v[2] >= 343.0f && v[2] <= 370.0f && v[1] >= 60.0f && v[1] <= 140.0f) {
						++havok;
						break;
					}
				}
			}
		}
		std::printf("sim.terrain G6 MP_007 RushLarge0: %zu points in the footprint (%zu in layer0_rush_logic, %zu without a parent reference), y %.3f..%.3f; "
					"terrain %.1f..%.1f m above (%zu open, %zu outside the grid); %zu disabled spawners (teams %zu); %zu Havok triangles in the box\n",
				points, in_logic, direct, double(y_lo), double(y_hi), double(depth_lo), double(depth_hi), open, outside, disabled_spawners, teams.size(), havok);
		CHECK(points == 24);
		CHECK(in_logic == 24);
		CHECK(y_lo >= 104.0f);
		CHECK(y_hi <= 104.5f);
		CHECK(open == 0);
		CHECK(outside == 0);
		CHECK(depth_lo >= 28.0f);
		CHECK(depth_hi <= 33.0f);
		CHECK(disabled_spawners == 2);
		CHECK(teams == std::set<int>{ 1, 2 });
		CHECK(havok == 0);
	}

	TEST_CASE("sim.terrain: the install is unchanged") {
		Shared *s = shared();
		if (!s->ok) {
			bf3::test::skip_install_case(s->reader.describe().c_str());
			return;
		}
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(s->before, s->reader.snapshot());
		CHECK_MESSAGE(d.clean(), d.summary());
		std::printf("sim.terrain install.unchanged: %s\n", d.clean() ? "yes" : "NO");
	}
}
