// Collision soup (bf3_core level/collision_soup.h). Suite collision_unit: synthetic shapes through
// the per-placement build (leaves, materials, surfaces, flags, quantisation, hashes, tiles), the
// word classifier and the hash bytes (the terrain heightfield's cases are in terrain_tests.cpp). Suite collision: the retail install (read-only; exits 77 =
// skipped without one): MP_Subway TeamDeathMatch0 and XP3_Alborz ConquestLarge0 against the
// numbers research/probes/tools/level-oracle `collision` prints through IW4L's crates (README
// "Collision overlay"), seeded mutations of Metro's static model group res through the soup, and
// the install's index files unchanged. Nothing from the install is stored.
#include <doctest/doctest.h>

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/res/res_loader.h"
#include "test_support.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <string>

using namespace bf3::core;
using namespace bf3::core::level;

namespace {

res::Shape box(const res::Vec3 &p_half, uint32_t p_user_data) {
	res::Shape s;
	s.type = res::ShapeType::Box;
	s.half_extents = p_half;
	s.user_data = p_user_data;
	return s;
}

std::vector<res::Shape> one(res::Shape p_shape) {
	std::vector<res::Shape> out;
	out.push_back(std::move(p_shape));
	return out;
}

res::Transform translation(float p_x, float p_y, float p_z) {
	return res::Transform::from_translation({ p_x, p_y, p_z });
}

std::string hex64(uint64_t p_value) {
	char text[24];
	std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(p_value));
	return text;
}

} // namespace

TEST_SUITE("collision_unit") {
	TEST_CASE("collision_unit: surface words follow IW4L's table and token rules") {
		CHECK(classify_surface("levels/mp_subway/props/concrete_barrier_01") == surface::kConcrete);
		CHECK(classify_surface("Glass_Window") == surface::kGlass); // case-insensitive
		CHECK(classify_surface("fx/impacts/metal/impact_metal_m") == surface::kMetal);
		CHECK(classify_surface("paintedmetal_door") == surface::kPaintedMetal); // the more specific word first
		CHECK(classify_surface("treeline") == surface::kFoliage); // "tree" starts the token
		CHECK(classify_surface("streetlamp") == std::nullopt); // a short word must start the token
		CHECK(classify_surface("woodenfence") == surface::kWood); // "wood" starts the token
		CHECK(classify_surface("rebar_reinforcedconcrete") == surface::kConcrete); // a long word anywhere in it
		CHECK(classify_surface("tiles_floor") == surface::kCeramic);
		CHECK(classify_surface("x9_ice") == surface::kIce);
		CHECK(classify_surface("") == std::nullopt);
		CHECK(classify_surface("\xc3\xa9glass") == surface::kGlass); // non-ASCII bytes separate tokens
		CHECK(surface_name(surface::kConcrete) == "concrete");
		CHECK(surface_name(99) == "none");
		CHECK(collision_body_name(1) == "raycast");
	}

	TEST_CASE("collision_unit: quantisation, degenerate, tiles and hash bytes") {
		std::array<int64_t, 9> q{};
		REQUIRE(soup_quantise({ { { 0.0005f, -0.0005f, 1.25f }, { -64.0f, 0.0f, 63.9f }, { 1e6f, 2.0f, -3.0f } } }, q));
		// round half away from zero (Rust f64::round): 0.0005 is just above or below in f32
		CHECK(q[0] == int64_t(std::round(double(0.0005f) * 1000.0)));
		CHECK(q[2] == 1250);
		CHECK(q[3] == -64000);
		CHECK(q[6] == 1000000000);
		CHECK_FALSE(soup_quantise({ { { std::numeric_limits<float>::quiet_NaN(), 0, 0 }, { 0, 0, 0 }, { 1, 1, 1 } } }, q));
		CHECK_FALSE(soup_quantise({ { { std::numeric_limits<float>::infinity(), 0, 0 }, { 0, 0, 0 }, { 1, 1, 1 } } }, q));
		CHECK_FALSE(soup_quantise({ { { 2e7f, 0, 0 }, { 0, 0, 0 }, { 1, 1, 1 } } }, q));
		std::array<int64_t, 9> d{ 1, 2, 3, 1, 2, 3, 7, 8, 9 };
		CHECK(soup_degenerate(d));
		d[3] = 2;
		CHECK_FALSE(soup_degenerate(d));
		// Centroid tiles: floor division, negative coordinates included.
		CHECK(soup_tile_of({ 0, 0, 0, 0, 0, 0, 0, 0, 0 }) == std::array<int32_t, 2>{ 0, 0 });
		CHECK(soup_tile_of({ -1, 0, 0, 0, 0, 0, 0, 0, 0 }) == std::array<int32_t, 2>{ -1, 0 });
		CHECK(soup_tile_of({ 64000, 0, 0, 64000, 0, 0, 64000, 0, -192001 }) == std::array<int32_t, 2>{ 1, -2 });
		CHECK(soup_tile_of({ 63999, 0, 0, 64000, 0, 0, 64000, 0, -192000 }) == std::array<int32_t, 2>{ 0, -1 });
		// FNV-1a 64 over 72 + 4 + 3 bytes; zero input pins the byte order and the prime.
		uint64_t h = kFnvOffset;
		soup_hash_bytes(h, {}, 0, 0, 0, 0);
		uint64_t ref = kFnvOffset;
		for (int i = 0; i < 79; ++i) {
			ref ^= 0;
			ref *= 0x00000100000001B3ull;
		}
		CHECK(h == ref);
		uint64_t a = kFnvOffset, b = kFnvOffset;
		soup_hash_bytes(a, { 1, 0, 0, 0, 0, 0, 0, 0, 0 }, 0, 0, 0, 0);
		soup_hash_bytes(b, { 256, 0, 0, 0, 0, 0, 0, 0, 0 }, 0, 0, 0, 0);
		CHECK(a != b); // little endian, every byte counts
	}

	TEST_CASE("collision_unit: a box placed in a frame, with its material and flags") {
		res::HavokPhysicsData havok;
		havok.material_flags_and_indices = { 7, kMaterialSeeThrough | 3 };
		const std::vector<res::Shape> roots = one(box({ 1.0f, 2.0f, 3.0f }, 0x0100)); // part 0, local material 1
		const SurfaceMaterials no_grid;
		const SoupShapeResult r = soup_shape_triangles(havok, roots, translation(100.0f, 5.0f, -100.0f), { uint8_t(CollisionBody::Raycast) }, no_grid,
				"levels/x/props/glass_window_invisiblecollision");
		REQUIRE(r.triangles.size() == 12);
		CHECK(r.destroyed == 0);
		CHECK(r.degenerate == 0);
		for (size_t i = 0; i < r.triangles.size(); ++i) {
			const SoupTriangle &t = r.triangles[i];
			CHECK(t.material == (kMaterialSeeThrough | 3));
			CHECK(t.surface == surface::kGlass); // no grid: the owner name decides
			CHECK(t.body == uint8_t(CollisionBody::Raycast));
			CHECK(t.flags == (kSoupSeeThrough | kSoupInvisible));
			for (const res::Vec3 &p : t.v) {
				CHECK(std::fabs(std::fabs(p[0] - 100.0f) - 1.0f) < 1e-5f);
				CHECK(std::fabs(std::fabs(p[1] - 5.0f) - 2.0f) < 1e-5f);
				CHECK(std::fabs(std::fabs(p[2] + 100.0f) - 3.0f) < 1e-5f);
			}
			// centroid near (100, -100): tile (1, -2)
			CHECK(r.tiles[i] == std::array<int32_t, 2>{ 1, -2 });
		}
		// A frame with a NaN drops every triangle as non-finite.
		res::Transform bad = translation(0.0f, 0.0f, 0.0f);
		bad.origin[1] = std::numeric_limits<float>::quiet_NaN();
		const SoupShapeResult n = soup_shape_triangles(havok, roots, bad, {}, no_grid, "");
		CHECK(n.triangles.empty());
		CHECK(n.nonfinite == 12);
		CHECK(n.hash == kFnvOffset);
	}

	TEST_CASE("collision_unit: group subparts, material indices and triangle subparts") {
		res::HavokPhysicsData havok;
		havok.material_flags_and_indices = { 11, 22, 33 };
		havok.material_indices = { 0, 0, 0, 2 }; // first part 2 + part 1 -> index 2
		res::Shape mesh;
		mesh.type = res::ShapeType::ExtendedMesh;
		mesh.extended = std::make_unique<res::ExtendedMesh>();
		res::ShapesSubpart instance;
		instance.rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
		instance.translation = { 10.0f, 0.0f, 0.0f };
		instance.user_data = 2; // first physics part of the instance
		instance.children.push_back(box({ 0.5f, 0.5f, 0.5f }, 0xFF01)); // local material 255: out of range
		mesh.extended->shape_subparts.push_back(std::move(instance));
		res::TrianglesSubpart tris;
		tris.vertices = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, 0.0001f } };
		tris.triangles = { { 0, 1, 2 }, { 0, 3, 1 }, { 0, 1, 9 } }; // a kept one, a degenerate one (1 mm), one past the vertices
		tris.materials = { 1, 0, 0 };
		mesh.extended->triangle_subparts.push_back(std::move(tris));
		std::vector<res::Shape> roots;
		roots.push_back(std::move(mesh));
		const SurfaceMaterials no_grid;
		const SoupShapeResult r = soup_shape_triangles(havok, roots, res::Transform::identity(), {}, no_grid, "");
		REQUIRE(r.triangles.size() == 13);
		CHECK(r.degenerate == 1);
		for (size_t i = 0; i < 12; ++i) {
			CHECK(r.triangles[i].material == 33); // MaterialIndices[first part + part]
			CHECK(r.triangles[i].surface == surface::kConcrete); // nothing names a surface
			CHECK(r.triangles[i].body == uint8_t(CollisionBody::Collision));
		}
		CHECK(r.triangles[12].material == 22); // the stored index
		CHECK(r.triangles[12].flags == 0);
		// The placement hash runs over the triangles in walk order; the sum does not depend on order.
		uint64_t stream = kFnvOffset, sum = 0;
		for (const SoupTriangle &t : r.triangles) {
			std::array<int64_t, 9> q{};
			REQUIRE(soup_quantise(t.v, q));
			soup_hash_bytes(stream, q, t.material, t.surface, t.body, t.flags);
			uint64_t h = kFnvOffset;
			soup_hash_bytes(h, q, t.material, t.surface, t.body, t.flags);
			sum += h;
		}
		CHECK(stream == r.hash);
		CHECK(sum == r.sum);
	}

	TEST_CASE("collision_unit: a mirroring frame keeps convex faces outward, deep trees end") {
		res::HavokPhysicsData havok;
		const SurfaceMaterials no_grid;
		const std::vector<res::Shape> roots = one(box({ 1.0f, 1.0f, 1.0f }, 0));
		res::Transform mirror = res::Transform::identity();
		mirror.axis[0] = { -1.0f, 0.0f, 0.0f };
		const SoupShapeResult r = soup_shape_triangles(havok, roots, mirror, {}, no_grid, "");
		REQUIRE(r.triangles.size() == 12);
		for (const SoupTriangle &t : r.triangles) {
			const res::Vec3 n = res::vec::cross(res::vec::sub(t.v[1], t.v[0]), res::vec::sub(t.v[2], t.v[0]));
			const res::Vec3 c = res::vec::scale(res::vec::add(res::vec::add(t.v[0], t.v[1]), t.v[2]), 1.0f / 3.0f);
			CHECK(res::vec::dot(n, c) > 0.0f); // outward from the box centre at the origin
		}
		// A list nested past the walk's depth limit (64) contributes nothing and returns.
		res::Shape deep = box({ 1.0f, 1.0f, 1.0f }, 0);
		for (int i = 0; i < 80; ++i) {
			res::Shape list;
			list.type = res::ShapeType::List;
			list.children.push_back(std::move(deep));
			deep = std::move(list);
		}
		std::vector<res::Shape> deep_roots;
		deep_roots.push_back(std::move(deep));
		CHECK(soup_shape_triangles(havok, deep_roots, res::Transform::identity(), {}, no_grid, "").triangles.empty());
	}
}

namespace {

std::shared_ptr<Install> open_or_skip(InstallReader &r_reader) {
	if (!r_reader.locate({})) {
		bf3::test::skip_install_case(r_reader.describe().c_str());
		return nullptr;
	}
	Result<std::shared_ptr<Install>> opened = Install::open(r_reader);
	REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
	return *opened;
}

struct Expected {
	const char *level;
	const char *mode;
	size_t placements;
	size_t triangles; // terrain included (the listing's totals: soup Havok + hash_soup_terrain)
	size_t terrain;
	const char *terrain_hash;
	size_t terrain_open; // cells left open (a hole corner); TC: of the whole grid
	size_t destroyed;
	size_t degenerate;
	size_t tiles;
	const char *sum;
	std::array<size_t, 3> bodies;
};

void check_level(const std::shared_ptr<Install> &p_install, const Expected &p_e) {
	const auto start = std::chrono::steady_clock::now();
	SoupProgress progress;
	SoupOptions options;
	options.progress = &progress;
	Result<LevelCollision> c = load_level_collision(p_install, p_e.level, p_e.mode, options);
	REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const CollisionSoup &s = c->soup;
	MESSAGE(std::string(p_e.level) << " " << std::string(p_e.mode) << ": " << s.triangles << " triangles, " << s.tiles.size() << " tiles in " << seconds << " s (soup "
						<< c->seconds_soup << " s)");
	CHECK(progress.stage.load() == SoupProgress::Done);
	CHECK(s.placements.size() == p_e.placements);
	CHECK(s.built == p_e.placements);
	CHECK(s.missing == 0);
	CHECK(s.hidden == 0);
	// TC: the soup holds the Havok triangles and the terrain record; the oracle's totals are both together, the
	// terrain's from hash_soup_terrain over the whole grid (as bf3dump collision prints them).
	REQUIRE(s.terrains.size() == 1);
	CHECK(s.terrains[0].state == SoupTerrain::State::Built);
	REQUIRE(s.terrains[0].grid != nullptr);
	CHECK(s.terrains[0].triangles == 0);
	WorkBudget budget(size_t(1) << 28);
	Result<SoupTerrainHash> th = hash_soup_terrain(s.terrains[0], uint32_t(s.placements.size()), budget);
	REQUIRE_MESSAGE(th.ok(), (th.ok() ? std::string() : th.error().message));
	CHECK(th->triangles == p_e.terrain);
	CHECK(s.terrain_triangles == p_e.terrain);
	CHECK(th->open == p_e.terrain_open);
	CHECK(s.terrains[0].open == p_e.terrain_open);
	CHECK(th->cells == s.terrains[0].cells);
	CHECK(hex64(th->hash) == p_e.terrain_hash);
	CHECK(s.triangles + th->triangles == p_e.triangles);
	CHECK(s.destroyed == p_e.destroyed);
	CHECK(s.degenerate + th->degenerate == p_e.degenerate);
	CHECK(s.nonfinite + th->nonfinite == 0);
	std::set<std::pair<int32_t, int32_t>> all_tiles;
	uint64_t sum = s.sum;
	for (const SoupTile &tile : s.tiles) {
		all_tiles.insert({ tile.x, tile.z });
	}
	for (const SoupTerrainTileSum &t : th->tiles) {
		all_tiles.insert({ t.tile[0], t.tile[1] });
		sum += t.sum;
	}
	CHECK(all_tiles.size() == p_e.tiles);
	CHECK(hex64(sum) == p_e.sum);
	std::array<size_t, 3> bodies = s.bodies;
	bodies[size_t(CollisionBody::Collision)] += th->triangles;
	CHECK(bodies == p_e.bodies);
	// Tiles: sorted, non-empty, every triangle's centroid inside its 64 m square, counts add up.
	size_t total = 0, outside = 0;
	std::set<std::pair<int32_t, int32_t>> seen;
	for (const SoupTile &tile : s.tiles) {
		REQUIRE_FALSE(tile.triangles.empty());
		CHECK(seen.insert({ tile.x, tile.z }).second);
		total += tile.triangles.size();
		for (const SoupTriangle &t : tile.triangles) {
			std::array<int64_t, 9> q{};
			if (!soup_quantise(t.v, q) || soup_tile_of(q) != std::array<int32_t, 2>{ tile.x, tile.z }) {
				++outside;
			}
		}
	}
	CHECK(total == s.triangles);
	CHECK(outside == 0);
	CHECK(std::is_sorted(s.tiles.begin(), s.tiles.end(), [](const SoupTile &a, const SoupTile &b) { return std::make_pair(a.x, a.z) < std::make_pair(b.x, b.z); }));
	CHECK_FALSE(c->spawns.empty());
}

} // namespace

TEST_SUITE("collision") {
	// level-oracle collision (TC, 2026-10-16, IW4L 49aca70 without its near-object rule: every terrain cell): placements,
	// triangles, terrain triangles, hash and open cells, destroyed, degenerate, tiles, sum, bodies. Each case also
	// checks that the install's index files are unchanged.
	TEST_CASE("collision: MP_Subway TeamDeathMatch0 matches the IW4L oracle") {
		InstallReader reader;
		const std::shared_ptr<Install> install = open_or_skip(reader);
		if (!install) {
			return;
		}
		const InstallSnapshot before = reader.snapshot();
		check_level(install, { "MP_Subway", "TeamDeathMatch0", 2122, 1136699, 196608, "308b8f1a3750c835", 163840, 72733, 34, 181, "082eac1e5df0ae1c",
									 { 991559, 142908, 2232 } });
		const SnapshotDiff diff = diff_snapshots(before, reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("collision: XP3_Alborz ConquestLarge0 matches the IW4L oracle") {
		InstallReader reader;
		const std::shared_ptr<Install> install = open_or_skip(reader);
		if (!install) {
			return;
		}
		const InstallSnapshot before = reader.snapshot();
		check_level(install, { "XP3_Alborz", "ConquestLarge0", 519, 36215517, 33554432, "f49b8d4219d957cf", 0, 56015, 0, 16384, "78096d54cf5022f0",
									 { 36106064, 109453, 0 } });
		const SnapshotDiff diff = diff_snapshots(before, reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("collision: mutated Havok res of Metro's static model groups never crash the soup") {
		InstallReader reader;
		const std::shared_ptr<Install> install = open_or_skip(reader);
		if (!install) {
			return;
		}
		Result<LevelCollision> c = load_level_collision(install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE(c.ok());
		const SurfaceMaterials no_grid;
		std::mt19937 rng(20261015u);
		size_t decoded = 0, failed = 0;
		for (const SoupPlacement &p : c->soup.placements) {
			if (p.kind != CollisionPlacement::Kind::Group) {
				continue;
			}
			Result<res::RawRes> raw = res::read_raw_res(c->level->mount(), p.res);
			REQUIRE(raw.ok());
			for (int mutant = 0; mutant < 16; ++mutant) {
				std::vector<uint8_t> bytes = raw->payload;
				const int flips = 1 + int(rng() % 16);
				for (int f = 0; f < flips && !bytes.empty(); ++f) {
					bytes[rng() % bytes.size()] ^= uint8_t(1u << (rng() % 8));
				}
				res::HavokPhysicsData data;
				if (!res::HavokPhysicsData::parse(res::Bytes(bytes.data(), bytes.size()), raw->res_meta, data).ok()) {
					++failed;
					continue;
				}
				std::vector<res::Shape> shapes;
				if (!data.shapes(shapes).ok()) {
					++failed;
					continue;
				}
				const SoupShapeResult r = soup_shape_triangles(data, shapes, res::Transform::identity(), {}, no_grid, "");
				CHECK(r.triangles.size() + r.degenerate + r.nonfinite < size_t(1) << 26);
				++decoded;
			}
		}
		MESSAGE("mutants: " << decoded << " built, " << failed << " refused by the decoders");
		CHECK(decoded + failed == 32);
	}
}
