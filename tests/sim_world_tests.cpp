#include <doctest/doctest.h>

#include "bf3/core/level/collision_soup.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/physics_world.h"
#include "bf3/sim/static_world.h"
#include "sim_world_support.h"
#include "test_support.h"

#include "collision_world_internal.h"
#include "static_world_internal.h"

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <thread>
#include <vector>

using namespace bf3;
using namespace bf3::sim;
namespace level = bf3::core::level;

using namespace sim_world_test;

TEST_SUITE("sim-world-unit") {
	TEST_CASE("sim.world: jolt build id names the pinned version and switches") {
		CHECK(jolt_build_id() == "5.6.0;det=1;dp=0");
	}

	TEST_CASE("sim.world: a floor answers rays from both sides with the face normal towards the query") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, -10.0f, -10.0f, 10.0f, 10.0f, 0.0f, tag_of(0x123456, 5, 0, level::kSoupInvisible));
		const auto shapes = build(tris);
		const auto world = world_of(shapes);

		StaticHit hit;
		REQUIRE(world->ray({ 1.0f, 5.0f, 2.0f }, { 0.0f, -10.0f, 0.0f }, query_mask::kMovement, &hit));
		CHECK(hit.fraction == doctest::Approx(0.5f).epsilon(1e-5));
		CHECK(hit.position[1] == doctest::Approx(0.0f).epsilon(1e-5));
		CHECK(hit.normal[0] == doctest::Approx(0.0f));
		CHECK(hit.normal[1] == doctest::Approx(1.0f));
		CHECK(hit.normal[2] == doctest::Approx(0.0f));
		CHECK(hit.layer == StaticLayer::Collision);
		CHECK(hit.tag == tag_of(0x123456, 5, 0, level::kSoupInvisible));
		CHECK(shapes->tile(hit.tile).x <= 0);

		REQUIRE(world->ray({ 1.0f, -5.0f, 2.0f }, { 0.0f, 10.0f, 0.0f }, query_mask::kAll, &hit));
		CHECK(hit.fraction == doctest::Approx(0.5f).epsilon(1e-5));
		CHECK(hit.normal[1] == doctest::Approx(-1.0f));

		CHECK_FALSE(world->ray({ 1.0f, 5.0f, 2.0f }, { 0.0f, -4.0f, 0.0f }, query_mask::kAll));
		CHECK_FALSE(world->ray({ 30.0f, 5.0f, 2.0f }, { 0.0f, -10.0f, 0.0f }, query_mask::kAll));
		CHECK_FALSE(world->ray({ 1.0f, 5.0f, 2.0f }, { 0.0f, 0.0f, 0.0f }, query_mask::kAll));
		CHECK_FALSE(world->ray({ 1.0f, std::numeric_limits<float>::quiet_NaN(), 2.0f }, { 0.0f, -10.0f, 0.0f }, query_mask::kAll));
		CHECK_FALSE(world->ray({ 1.0f, 5.0f, 2.0f }, { 0.0f, -10.0f, 0.0f }, 0));
	}

	TEST_CASE("sim.world: the up-wound floor's front face points up in Jolt (soup winding goes in as stored)") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, 1.0f, 1.0f, 9.0f, 9.0f, 2.0f);
		const auto shapes = build(tris);
		const StaticWorldShapes::Impl &s = shapes->impl();
		REQUIRE(s.tiles.size() == 1);
		const JPH::Shape *mesh = s.tiles[0].shapes[0].GetPtr();
		REQUIRE(mesh != nullptr);

		const auto world = world_of(shapes);
		const JPH::RayCastSettings front_only;
		JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> down;
		world->impl().system->GetNarrowPhaseQueryNoLock().CastRay(JPH::RRayCast(JPH::RVec3(5.0f, 10.0f, 5.0f), JPH::Vec3(0.0f, -20.0f, 0.0f)), front_only, down);
		CHECK(down.HadHit());
		JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> up;
		world->impl().system->GetNarrowPhaseQueryNoLock().CastRay(JPH::RRayCast(JPH::RVec3(5.0f, -10.0f, 5.0f), JPH::Vec3(0.0f, 20.0f, 0.0f)), front_only, up);
		CHECK_FALSE(up.HadHit());
	}

	TEST_CASE("sim.world: tiles follow the soup's rule and carry counts, tags, bounds and content digests") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, 1.0f, 1.0f, 9.0f, 9.0f, 0.0f, tag_of(1, 1));
		add_floor(tris, 70.0f, 1.0f, 80.0f, 9.0f, 0.0f, tag_of(2, 2));
		add_floor(tris, -150.0f, -10.0f, -140.0f, -2.0f, 1.0f, tag_of(3, 3));
		add_wall_x(tris, 5.0f, 2.0f, 8.0f, 0.0f, 3.0f, tag_of(4, 4, 1), StaticLayer::Raycast);
		add_wall_x(tris, 6.0f, 2.0f, 8.0f, 0.0f, 3.0f, tag_of(1, 1, 2), StaticLayer::Character);
		std::atomic<size_t> done{ 0 };
		StaticWorldBuildOptions options;
		options.tiles_done = &done;
		options.threads = 2;
		const auto shapes = build(tris, options);
		CHECK(done.load() == 3);
		REQUIRE(shapes->tile_count() == 3);
		CHECK(shapes->tile(0).x == -3);
		CHECK(shapes->tile(0).z == -1);
		CHECK(shapes->tile(1).x == 0);
		CHECK(shapes->tile(1).z == 0);
		CHECK(shapes->tile(2).x == 1);
		CHECK(shapes->tile(2).z == 0);
		const StaticTileInfo &t0 = shapes->tile(1);
		CHECK(t0.triangles[0] == 2);
		CHECK(t0.triangles[1] == 2);
		CHECK(t0.triangles[2] == 2);
		CHECK(t0.tags == 3);
		CHECK(shapes->tag(1, 0) == tag_of(1, 1));
		CHECK(shapes->tag(1, 1) == tag_of(4, 4, 1));
		CHECK(shapes->tag(1, 2) == tag_of(1, 1, 2));
		CHECK(shapes->tag(1, 3) == SurfaceTag());
		CHECK(shapes->tag(99, 0) == SurfaceTag());
		CHECK(shapes->tile(99).triangles[0] == 0);
		CHECK(t0.min[0] == 1.0f);
		CHECK(t0.max[0] == 9.0f);
		CHECK(t0.max[1] == 3.0f);
		CHECK(shapes->min()[0] == -150.0f);
		CHECK(shapes->max()[0] == 80.0f);
		CHECK(shapes->tile(0).content != shapes->tile(1).content);
		CHECK(shapes->tile(1).content != shapes->tile(2).content);
		const StaticWorldStats &st = shapes->stats();
		CHECK(st.tiles == 3);
		CHECK(st.shapes == 5);
		CHECK(st.triangles[0] == 6);
		CHECK(st.triangles[1] == 2);
		CHECK(st.triangles[2] == 2);
		CHECK(st.tags == 5);
		CHECK(st.tiles_cooked == 3);
		CHECK(st.tiles_hit == 0);
		CHECK_FALSE(st.world_hit);
		CHECK(st.shape_bytes > 0);

		const auto again = build(tris);
		for (size_t i = 0; i < 3; ++i) {
			CHECK(again->tile(i).content == shapes->tile(i).content);
		}
		std::vector<StaticTriangle> moved = tris;
		moved[0].v[0][1] = std::nextafter(0.0f, 1.0f);
		CHECK(build(moved)->tile(1).content != shapes->tile(1).content);
		std::vector<StaticTriangle> retagged = tris;
		retagged[0].tag.flags = 1;
		CHECK(build(retagged)->tile(1).content != shapes->tile(1).content);
		std::vector<StaticTriangle> relayered = tris;
		relayered[0].layer = StaticLayer::Character;
		CHECK(build(relayered)->tile(1).content != shapes->tile(1).content);
		CHECK(build(relayered)->tile(0).content == shapes->tile(0).content);

		StaticWorldBuildOptions no_ray;
		no_ray.include_raycast = false;
		const auto lean = build(tris, no_ray);
		CHECK(lean->tile(1).triangles[1] == 0);
		CHECK(lean->stats().shapes == 4);
		CHECK(lean->tile(1).content == shapes->tile(1).content);
	}

	TEST_CASE("sim.world: query masks select the layers") {
		std::vector<StaticTriangle> tris;
		add_wall_x(tris, 2.0f, -5.0f, 5.0f, 0.0f, 5.0f, tag_of(10, 1, 1), StaticLayer::Raycast);
		add_wall_x(tris, 4.0f, -5.0f, 5.0f, 0.0f, 5.0f, tag_of(20, 2, 2), StaticLayer::Character);
		add_wall_x(tris, 6.0f, -5.0f, 5.0f, 0.0f, 5.0f, tag_of(30, 3, 0), StaticLayer::Collision);
		const auto world = world_of(build(tris));
		const Vec3 from{ 0.0f, 1.0f, 0.0f };
		const Vec3 along{ 10.0f, 0.0f, 0.0f };
		StaticHit hit;
		REQUIRE(world->ray(from, along, query_mask::kAll, &hit));
		CHECK(hit.layer == StaticLayer::Raycast);
		CHECK(hit.tag.material == 10);
		CHECK(hit.position[0] == doctest::Approx(2.0f));
		CHECK(hit.normal[0] == doctest::Approx(-1.0f));
		REQUIRE(world->ray(from, along, query_mask::kMovement, &hit));
		CHECK(hit.layer == StaticLayer::Character);
		CHECK(hit.tag.material == 20);
		REQUIRE(world->ray(from, along, query_mask::kProjectile, &hit));
		CHECK(hit.layer == StaticLayer::Raycast);
		REQUIRE(world->ray(from, along, query_mask::kCollision, &hit));
		CHECK(hit.layer == StaticLayer::Collision);
		CHECK(hit.tag.material == 30);
		CHECK(hit.position[0] == doctest::Approx(6.0f));

		REQUIRE(world->ray(from, along, query_mask::kCollision | 0xF0u, &hit));
		CHECK(hit.layer == StaticLayer::Collision);

		CHECK(world->overlap_capsule({ 4.0f, 0.5f, 0.0f }, 0.3f, 1.8f, query_mask::kMovement));
		CHECK_FALSE(world->overlap_capsule({ 4.0f, 0.5f, 0.0f }, 0.3f, 1.8f, query_mask::kProjectile));
		CHECK(world->overlap_capsule({ 2.0f, 0.5f, 0.0f }, 0.3f, 1.8f, query_mask::kProjectile));
		CHECK_FALSE(world->overlap_capsule({ 2.0f, 0.5f, 0.0f }, 0.3f, 1.8f, query_mask::kMovement));
	}

	TEST_CASE("sim.world: capsule sweeps stop at the first contact; a start in contact reports fraction 0") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, -20.0f, -20.0f, 20.0f, 20.0f, 0.0f, tag_of(7, 7));
		add_wall_x(tris, 5.0f, -20.0f, 20.0f, 0.0f, 10.0f, tag_of(8, 8));
		const auto world = world_of(build(tris));
		const float r = 0.4f;
		const float h = 1.8f;
		StaticHit hit;

		REQUIRE(world->sweep_capsule({ 0.0f, 2.0f, 0.0f }, r, h, { 0.0f, -4.0f, 0.0f }, query_mask::kMovement, &hit));
		CHECK(hit.fraction == doctest::Approx(0.5f).epsilon(1e-3));
		CHECK(hit.position[1] == doctest::Approx(0.0f).epsilon(1e-3));
		CHECK(hit.normal[1] == doctest::Approx(1.0f));
		CHECK(hit.tag == tag_of(7, 7));

		REQUIRE(world->sweep_capsule({ 0.0f, 1.0f, 0.0f }, r, h, { 9.2f, 0.0f, 0.0f }, query_mask::kMovement, &hit));
		CHECK(hit.fraction == doctest::Approx(0.5f).epsilon(1e-3));
		CHECK(hit.position[0] == doctest::Approx(5.0f).epsilon(1e-3));
		CHECK(hit.normal[0] == doctest::Approx(-1.0f));
		CHECK(hit.tag == tag_of(8, 8));

		CHECK_FALSE(world->sweep_capsule({ 0.0f, 1.0f, 0.0f }, r, h, { -3.0f, 1.0f, 0.0f }, query_mask::kMovement));

		REQUIRE(world->sweep_capsule({ 0.0f, -0.05f, 0.0f }, r, h, { 1.0f, 0.0f, 0.0f }, query_mask::kMovement, &hit));
		CHECK(hit.fraction == 0.0f);

		CHECK(world->sweep_capsule({ 0.0f, -0.05f, 0.0f }, r, h, { 0.0f, 0.0f, 0.0f }, query_mask::kMovement, &hit));
		CHECK(hit.fraction == 0.0f);
		CHECK_FALSE(world->sweep_capsule({ 0.0f, 1.0f, 0.0f }, r, h, { 0.0f, 0.0f, 0.0f }, query_mask::kMovement));

		CHECK(world->sweep_capsule({ 0.0f, 1.0f, 0.0f }, r, 2.0f * r, { 0.0f, -2.0f, 0.0f }, query_mask::kMovement));
		CHECK_FALSE(world->sweep_capsule({ 0.0f, 1.0f, 0.0f }, 0.0f, h, { 0.0f, -2.0f, 0.0f }, query_mask::kMovement));
		CHECK_FALSE(world->sweep_capsule({ 0.0f, 1.0f, 0.0f }, r, r, { 0.0f, -2.0f, 0.0f }, query_mask::kMovement));
		CHECK_FALSE(world->sweep_capsule({ 0.0f, 1.0f, 0.0f }, std::numeric_limits<float>::infinity(), h, { 0.0f, -2.0f, 0.0f }, query_mask::kMovement));
		CHECK_FALSE(world->sweep_capsule({ 0.0f, 1.0f, 0.0f }, r, h, { 0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f }, query_mask::kMovement));
	}

	TEST_CASE("sim.world: capsule and box overlaps honour the tolerance and the yaw") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, -20.0f, -20.0f, 20.0f, 20.0f, 0.0f);
		add_wall_x(tris, 5.0f, -20.0f, 20.0f, 0.0f, 10.0f);
		const auto world = world_of(build(tris));
		CHECK_FALSE(world->overlap_capsule({ 0.0f, 0.01f, 0.0f }, 0.4f, 1.8f, query_mask::kMovement));
		CHECK(world->overlap_capsule({ 0.0f, -0.05f, 0.0f }, 0.4f, 1.8f, query_mask::kMovement));
		CHECK_FALSE(world->overlap_capsule({ 0.0f, -0.05f, 0.0f }, 0.4f, 1.8f, query_mask::kMovement, 0.1f));
		CHECK(world->overlap_capsule({ 4.7f, 1.0f, 0.0f }, 0.4f, 1.8f, query_mask::kMovement));
		CHECK_FALSE(world->overlap_capsule({ 0.0f, 1.0f, 0.0f }, -1.0f, 1.8f, query_mask::kMovement));

		const Vec3 half{ 0.1f, 0.2f, 1.0f };
		const Vec3 centre{ 4.5f, 1.0f, 0.0f };
		CHECK_FALSE(world->overlap_box(centre, half, 0.0f, query_mask::kMovement));
		CHECK(world->overlap_box(centre, half, 1.5707963f, query_mask::kMovement));
		CHECK(world->overlap_box(centre, half, 0.7853982f, query_mask::kMovement));
		CHECK_FALSE(world->overlap_box(centre, half, 0.7853982f, query_mask::kMovement, 0.3f));
		CHECK_FALSE(world->overlap_box(centre, { 0.0f, 0.2f, 1.0f }, 0.0f, query_mask::kMovement));
		CHECK_FALSE(world->overlap_box(centre, half, std::numeric_limits<float>::quiet_NaN(), query_mask::kMovement));

		CHECK_FALSE(world->overlap_box({ 0.0f, 0.2001f, 0.0f }, half, 0.3f, query_mask::kMovement));
		CHECK(world->overlap_box({ 0.0f, 0.18f, 0.0f }, half, 0.3f, query_mask::kMovement));
	}

	TEST_CASE("sim.world: ground_below finds the highest surface under a point within the drop") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, -20.0f, -20.0f, 20.0f, 20.0f, 0.0f, tag_of(1, 1));
		add_floor(tris, 2.0f, 2.0f, 4.0f, 4.0f, 1.5f, tag_of(2, 2));
		const auto world = world_of(build(tris));
		StaticHit hit;
		REQUIRE(world->ground_below({ 3.0f, 5.0f, 3.0f }, 10.0f, query_mask::kMovement, &hit));
		CHECK(hit.position[1] == doctest::Approx(1.5f));
		CHECK(hit.tag == tag_of(2, 2));
		CHECK(hit.normal[1] == doctest::Approx(1.0f));
		REQUIRE(world->ground_below({ 3.0f, 1.0f, 3.0f }, 10.0f, query_mask::kMovement, &hit));
		CHECK(hit.position[1] == doctest::Approx(0.0f));
		CHECK(hit.tag == tag_of(1, 1));
		CHECK_FALSE(world->ground_below({ 0.0f, 5.0f, 0.0f }, 4.0f, query_mask::kMovement, &hit));
		CHECK_FALSE(world->ground_below({ 0.0f, 5.0f, 0.0f }, 0.0f, query_mask::kMovement, &hit));
		CHECK_FALSE(world->ground_below({ 0.0f, 5.0f, 0.0f }, -1.0f, query_mask::kMovement, &hit));
		CHECK(world->ground_below({ 0.0f, 5.0f, 0.0f }, 6.0f, query_mask::kMovement, nullptr));
	}

	TEST_CASE("sim.world: two worlds over one build and two builds of one input answer bit-identically") {
		const std::vector<StaticTriangle> field = bumpy_field(100.0f);
		const auto shapes = build(field);
		CHECK(shapes->tile_count() == 16);
		const auto shapes_again = build(field, StaticWorldBuildOptions{ 1 });
		REQUIRE(shapes_again->tile_count() == 16);
		const auto a = world_of(shapes);
		const auto b = world_of(shapes);
		const auto c = world_of(shapes_again);
		CHECK(a->shapes() == b->shapes());
		std::mt19937 rng(1234);
		std::uniform_real_distribution<float> pos(-110.0f, 110.0f);
		std::uniform_real_distribution<float> dir(-1.0f, 1.0f);
		size_t hits = 0;
		size_t sweeps = 0;
		for (int i = 0; i < 1000; ++i) {
			const Vec3 origin{ pos(rng), 4.0f + 2.0f * dir(rng), pos(rng) };
			const Vec3 delta{ 20.0f * dir(rng), -8.0f + 2.0f * dir(rng), 20.0f * dir(rng) };
			StaticHit ha, hb, hc;
			const bool ra = a->ray(origin, delta, query_mask::kAll, &ha);
			REQUIRE(ra == b->ray(origin, delta, query_mask::kAll, &hb));
			REQUIRE(ra == c->ray(origin, delta, query_mask::kAll, &hc));
			if (ra) {
				++hits;
				CHECK(bits_equal(ha, hb));
				CHECK(bits_equal(ha, hc));
				CHECK(ha.tag.material < 7);
				CHECK(ha.tag.surface < 3);
			}
			const bool sa = a->sweep_capsule(origin, 0.4f, 1.8f, delta, query_mask::kMovement, &ha);
			REQUIRE(sa == c->sweep_capsule(origin, 0.4f, 1.8f, delta, query_mask::kMovement, &hc));
			if (sa) {
				++sweeps;
				CHECK(bits_equal(ha, hc));
			}
		}
		MESSAGE("1000 seeded rays: ", hits, " hits; capsule sweeps: ", sweeps, " hits");
		CHECK(hits > 500);
		CHECK(sweeps > 500);
	}

	TEST_CASE("sim.world: worlds over shared shapes run on two threads at once") {
		const auto shapes = build(bumpy_field(60.0f));
		const auto run = [&](uint32_t p_seed, std::vector<float> &r_out) {
			const auto world = world_of(shapes);
			std::mt19937 rng(p_seed);
			std::uniform_real_distribution<float> pos(-60.0f, 60.0f);
			for (int i = 0; i < 2000; ++i) {
				StaticHit hit;
				r_out.push_back(world->ground_below({ pos(rng), 10.0f, pos(rng) }, 20.0f, query_mask::kMovement, &hit) ? hit.position[1] : -1e9f);
			}
		};
		std::vector<float> one, two, reference;
		std::thread t1([&] { run(7, one); });
		std::thread t2([&] { run(7, two); });
		t1.join();
		t2.join();
		run(7, reference);
		CHECK(one == reference);
		CHECK(two == reference);
	}

	TEST_CASE("sim.world: a soup and its loose triangles build the same tiles") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, 1.0f, 1.0f, 9.0f, 9.0f, 0.0f, tag_of(5, 1, 0, level::kSoupTerrain));
		add_wall_x(tris, 70.0f, 1.0f, 9.0f, 0.0f, 3.0f, tag_of(6, 2, 2), StaticLayer::Character);
		level::CollisionSoup soup;
		for (int k = 0; k < 2; ++k) {
			level::SoupTile tile;
			tile.x = k;
			tile.z = 0;
			for (size_t i = 0; i < 2; ++i) {
				const StaticTriangle &t = tris[size_t(k) * 2 + i];
				level::SoupTriangle s;
				s.v = t.v;
				s.material = t.tag.material;
				s.surface = t.tag.surface;
				s.body = t.tag.body;
				s.flags = t.tag.flags;
				tile.triangles.push_back(s);
			}
			soup.tiles.push_back(tile);
		}
		const core::Result<std::shared_ptr<const StaticWorldShapes>> from_soup = build_static_world(soup);
		REQUIRE(from_soup.ok());
		const auto from_span = build(tris);
		REQUIRE((*from_soup)->tile_count() == from_span->tile_count());
		for (size_t i = 0; i < from_span->tile_count(); ++i) {
			CHECK((*from_soup)->tile(i).x == from_span->tile(i).x);
			CHECK((*from_soup)->tile(i).content == from_span->tile(i).content);
			CHECK((*from_soup)->tile(i).triangles == from_span->tile(i).triangles);
		}

		soup.tiles[0].triangles[0].body = 9;
		CHECK_FALSE(build_static_world(soup).ok());
	}

	TEST_CASE("sim.world: hostile input fails with an error and degenerate input builds no shape") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, 1.0f, 1.0f, 9.0f, 9.0f, 0.0f);
		std::vector<StaticTriangle> nan = tris;
		nan[1].v[2][0] = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(build_static_world(std::span<const StaticTriangle>(nan)).ok());
		std::vector<StaticTriangle> far = tris;
		far[0].v[1][2] = 1e8f;
		CHECK_FALSE(build_static_world(std::span<const StaticTriangle>(far)).ok());
		std::vector<StaticTriangle> layer = tris;
		layer[0].layer = StaticLayer(7);
		CHECK_FALSE(build_static_world(std::span<const StaticTriangle>(layer)).ok());
		StaticWorldBuildOptions bad;
		bad.active_edge_degrees = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(build_static_world(std::span<const StaticTriangle>(tris), bad).ok());
		bad.active_edge_degrees = -1.0f;
		CHECK_FALSE(build_static_world(std::span<const StaticTriangle>(tris), bad).ok());
		CHECK_FALSE(CollisionWorld::create(nullptr).ok());
		CollisionWorldOptions tiny;
		tiny.temp_allocator_bytes = 16;
		CHECK_FALSE(CollisionWorld::create(build(tris), tiny).ok());

		std::vector<StaticTriangle> degenerate(1);
		degenerate[0].v = { Vec3{ 1.0f, 0.0f, 1.0f }, Vec3{ 1.0f, 0.0f, 1.0f }, Vec3{ 2.0f, 0.0f, 1.0f } };
		const auto empty_tile = build(degenerate);
		REQUIRE(empty_tile->tile_count() == 1);
		CHECK(empty_tile->tile(0).triangles[0] == 0);
		CHECK(empty_tile->stats().shapes == 0);
		const auto w = world_of(empty_tile);
		CHECK_FALSE(w->ray({ 1.5f, 1.0f, 1.0f }, { 0.0f, -2.0f, 0.0f }, query_mask::kAll));

		const auto none = build({});
		CHECK(none->tile_count() == 0);
		const auto empty = world_of(none);
		CHECK_FALSE(empty->ray({ 0.0f, 1.0f, 0.0f }, { 0.0f, -2.0f, 0.0f }, query_mask::kAll));
		CHECK_FALSE(empty->overlap_capsule({ 0.0f, 0.0f, 0.0f }, 0.4f, 1.8f, query_mask::kAll));
	}

	TEST_CASE("sim.world: the Jolt runtime is shared with PhysicsWorld in any order") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, 1.0f, 1.0f, 9.0f, 9.0f, 0.0f);
		auto shapes = build(tris);
		{
			PhysicsWorld physics;
			REQUIRE(physics.init(WorldConfig{}));
			auto world = world_of(shapes);
			REQUIRE(physics.step());
			physics.shutdown();
			CHECK(world->ray({ 2.0f, 1.0f, 2.0f }, { 0.0f, -2.0f, 0.0f }, query_mask::kAll));
		}
		auto world = world_of(shapes);
		shapes.reset();
		CHECK(world->ray({ 2.0f, 1.0f, 2.0f }, { 0.0f, -2.0f, 0.0f }, query_mask::kAll));
		world.reset();
		PhysicsWorld physics;
		REQUIRE(physics.init(WorldConfig{}));
		CHECK(physics.step());
	}

	TEST_CASE("sim.world: the Impl seam carries a CharacterVirtual (filters, system, temp allocator, describe_hit)") {
		std::vector<StaticTriangle> tris;
		add_floor(tris, -10.0f, -10.0f, 10.0f, 10.0f, 0.0f, tag_of(77, 3));
		add_wall_x(tris, 3.0f, -10.0f, 10.0f, 0.0f, 3.0f, tag_of(88, 4, 2), StaticLayer::Character);
		add_wall_x(tris, -3.0f, -10.0f, 10.0f, 0.0f, 3.0f, tag_of(99, 5, 1), StaticLayer::Raycast);
		const auto world = world_of(build(tris));
		CollisionWorld::Impl &w = world->impl();
		REQUIRE(w.system != nullptr);
		REQUIRE(w.temp != nullptr);
		CHECK(w.bodies.size() == 6);
		CHECK(w.broadphase_filters.size() == 8);
		CHECK(w.object_filters.size() == 8);
		CHECK(w.object_filter(query_mask::kMovement).ShouldCollide(JPH::ObjectLayer(StaticLayer::Collision)));
		CHECK(w.object_filter(query_mask::kMovement).ShouldCollide(JPH::ObjectLayer(StaticLayer::Character)));
		CHECK_FALSE(w.object_filter(query_mask::kMovement).ShouldCollide(JPH::ObjectLayer(StaticLayer::Raycast)));
		CHECK_FALSE(w.object_filter(query_mask::kAll).ShouldCollide(kQueryObjectLayer));
		CHECK(w.object_pairs->ShouldCollide(kQueryObjectLayer, 0));
		CHECK_FALSE(w.object_pairs->ShouldCollide(0, 2));

		JPH::CharacterVirtualSettings settings;
		settings.mShape = new JPH::CapsuleShape(0.5f, 0.4f);
		settings.mShapeOffset = JPH::Vec3(0.0f, 0.9f, 0.0f);
		settings.mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
		settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
		settings.SetEmbedded();
		JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(&settings, JPH::RVec3(0.0f, 0.0f, 0.0f), JPH::Quat::sIdentity(), 0, w.system.get());
		const uint32_t mask = query_mask::kMovement;
		character->RefreshContacts(w.broadphase_filter(mask), w.object_filter(mask), w.body_filter, w.shape_filter, *w.temp);
		CHECK(character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround);
		StaticHit ground;
		w.describe_hit(character->GetGroundBodyID(), character->GetGroundSubShapeID(), ground);
		CHECK(ground.layer == StaticLayer::Collision);
		CHECK(ground.tag == tag_of(77, 3));

		JPH::CharacterVirtual::ExtendedUpdateSettings update;
		for (int i = 0; i < 120; ++i) {
			character->SetLinearVelocity(JPH::Vec3(4.0f, 0.0f, 0.0f));
			character->ExtendedUpdate(kTickSeconds, JPH::Vec3(0.0f, -9.81f, 0.0f), update, w.broadphase_filter(mask), w.object_filter(mask), w.body_filter,
					w.shape_filter, *w.temp);
		}
		CHECK(float(character->GetPosition().GetX()) == doctest::Approx(2.6f).epsilon(0.01));
		for (int i = 0; i < 180; ++i) {
			character->SetLinearVelocity(JPH::Vec3(-4.0f, 0.0f, 0.0f));
			character->ExtendedUpdate(kTickSeconds, JPH::Vec3(0.0f, -9.81f, 0.0f), update, w.broadphase_filter(mask), w.object_filter(mask), w.body_filter,
					w.shape_filter, *w.temp);
		}
		CHECK(float(character->GetPosition().GetX()) < -3.5f);
		CHECK(float(character->GetPosition().GetY()) == doctest::Approx(0.0f).epsilon(0.01));
	}
}
