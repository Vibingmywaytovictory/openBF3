// Our own Jolt world: fixed-step, deterministic, settles on the ground. Recast/Detour link.
#include <doctest/doctest.h>

#include "bf3/sim/nav_link.h"
#include "bf3/sim/physics_world.h"

#include <string>
#include <vector>

using bf3::sim::PhysicsWorld;
using bf3::sim::WorldConfig;

namespace {

std::vector<uint64_t> run_hashes(const WorldConfig &p_config, int p_steps, int p_every) {
	PhysicsWorld w;
	REQUIRE(w.init(p_config));
	std::vector<uint64_t> hashes{ w.state_hash() };
	for (int i = 1; i <= p_steps; ++i) {
		REQUIRE(w.step());
		if (i % p_every == 0) {
			hashes.push_back(w.state_hash());
		}
	}
	return hashes;
}

} // namespace

TEST_CASE("sim: Jolt world steps deterministically twice with identical results") {
	WorldConfig cfg;
	cfg.dynamic_bodies = 8;
	cfg.job_threads = 2;
	const std::vector<uint64_t> a = run_hashes(cfg, 600, 30);
	const std::vector<uint64_t> b = run_hashes(cfg, 600, 30);
	REQUIRE(a.size() == b.size());
	CHECK(a == b);
	CHECK(a.front() != a[1]); // bodies moved
	MESSAGE("Jolt ", std::string(PhysicsWorld::jolt_version()), ", 600 steps, final hash ", a.back());
	CHECK(std::string(PhysicsWorld::jolt_version()) == "5.6.0");
}

TEST_CASE("sim: result does not depend on the job thread count") {
	WorldConfig one;
	one.job_threads = 0; // single-threaded job system
	WorldConfig four;
	four.job_threads = 4;
	CHECK(run_hashes(one, 300, 50) == run_hashes(four, 300, 50));
}

TEST_CASE("sim: bodies fall and come to rest on the ground") {
	PhysicsWorld w;
	WorldConfig cfg;
	REQUIRE(w.init(cfg));
	CHECK(w.dynamic_body_count() == cfg.dynamic_bodies);
	CHECK(w.active_body_count() == cfg.dynamic_bodies);
	for (int i = 0; i < 60 * 10; ++i) {
		REQUIRE(w.step());
	}
	CHECK(w.tick() == 600);
	CHECK(w.lowest_dynamic_y() > 0.3f); // resting on the ground (top at y = 0), radius/half size 0.5
	CHECK(w.lowest_dynamic_y() < 0.7f);
	MESSAGE("active bodies after 10 s: ", w.active_body_count());
	std::vector<bf3::sim::BodyTransform> xf;
	w.get_dynamic_transforms(xf);
	CHECK(xf.size() == static_cast<size_t>(cfg.dynamic_bodies));
}

TEST_CASE("sim: two worlds can live at once (shared Jolt runtime)") {
	PhysicsWorld a;
	PhysicsWorld b;
	REQUIRE(a.init({}));
	REQUIRE(b.init({}));
	for (int i = 0; i < 120; ++i) {
		REQUIRE(a.step());
		REQUIRE(b.step());
	}
	CHECK(a.state_hash() == b.state_hash());
	a.shutdown();
	CHECK(b.step());
}

TEST_CASE("sim: Recast/Detour are linked and work") {
	const bf3::sim::NavLinkResult r = bf3::sim::nav_link_check();
	MESSAGE("recast grid ", r.grid_w, "x", r.grid_h, ", spans ", r.spans);
	CHECK(r.ok);
}
