#include "bf3/core/gameplay/death_body.h"
#include "bf3/core/res/havok_ragdoll.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/sim/ragdoll_world.h"
#include "sim_world_support.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace bf3;
using namespace bf3::sim;
using namespace sim_world_test;

namespace {

std::shared_ptr<const core::res::RagdollDef> load_ragdoll() {
	test::Mounted *m = test::shared_mount_or_skip();
	if (m == nullptr) {
		return nullptr;
	}
	core::Result<core::LevelMount> mount = m->install->mount_superbundle("Win32/Levels/MP_001/MP_001");
	REQUIRE(mount.ok());
	REQUIRE(mount->load_all_bundles().ok());
	auto res = mount->read_res("animations/settings/ragdolls/ragdoll01");
	REQUIRE(res.ok());
	auto def = std::make_shared<core::res::RagdollDef>();
	const core::res::ResError e = core::res::RagdollDef::parse(res->second, res->first.res_meta, *def);
	REQUIRE_MESSAGE(e.ok(), e.message());
	return def;
}

std::shared_ptr<const StaticWorldShapes> floor_world() {
	std::vector<StaticTriangle> tris;
	add_floor(tris, -20.0f, -20.0f, 20.0f, 20.0f, 0.0f);
	core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(tris);
	REQUIRE(shapes.ok());
	return *shapes;
}

std::vector<RagdollBodyPose> rest_pose(const core::res::RagdollDef &p_def, float p_lift) {
	std::vector<RagdollBodyPose> out;
	for (const core::res::RagdollBodyDef &b : p_def.bodies) {
		RagdollBodyPose p;
		p.position = Vec3{ b.rest.trans.x, b.rest.trans.y + p_lift, b.rest.trans.z };
		p.rotation = b.rotation;
		out.push_back(p);
	}
	return out;
}

float joint_gap(const core::res::RagdollDef &p_def, const core::res::RagdollJointDef &p_joint,
		std::span<const RagdollBodyPose> p_pose) {
	auto world = [&](uint32_t p_body, const core::Vec3 &p_local) {
		const core::Transform t =
				core::Transform::from_rotation_translation(p_pose[p_body].rotation, p_pose[p_body].position);
		return t.transform_point(p_local);
	};
	return core::vec::length(core::vec::sub(
			world(p_joint.child, p_joint.child_frame.trans), world(p_joint.parent, p_joint.parent_frame.trans)));
}

}

TEST_SUITE("sim-ragdoll") {
	TEST_CASE("ragdoll01 in Jolt: a soldier let go standing on a floor falls, keeps its joints and freezes by the "
			  "R5-DT2 C rule") {
		std::shared_ptr<const core::res::RagdollDef> def = load_ragdoll();
		if (def == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<RagdollWorld>> world = RagdollWorld::create(floor_world(), def);
		REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
		RagdollWorld &w = **world;
		CHECK(w.body_count() == 15);
		CHECK(std::count(w.parents().begin(), w.parents().end(), UINT32_MAX) == 1);
		const std::vector<RagdollBodyPose> start = rest_pose(*def, 0.05f);
		std::vector<Vec3> push(def->bodies.size(), Vec3{ 0.0f, 0.0f, 0.0f });
		push[size_t(def->find_body("Head"))] = Vec3{ 0.0f, 0.0f, 1.5f };
		REQUIRE(w.add(7, start, push, {}).ok());
		CHECK_FALSE(w.add(7, start, {}, {}).ok());
		CHECK(w.has(7));
		core::gameplay::RagdollSettle settle;
		const float dt = 1.0f / 60.0f;
		int frozen_at = -1;
		float worst_gap = 0.0f;
		std::vector<RagdollBodyPose> pose(def->bodies.size());
		for (int i = 1; i <= 600 && frozen_at < 0; ++i) {
			REQUIRE(w.step(dt).ok());
			REQUIRE(w.pose(7, pose));
			for (const core::res::RagdollJointDef &j : def->joints) {
				worst_gap = std::max(worst_gap, joint_gap(*def, j, pose));
			}
			if (settle.update(dt, w.max_linear_speed(7), w.max_angular_speed(7))) {
				w.freeze(7);
				frozen_at = i;
			}
		}
		CHECK(frozen_at > 30);
		CHECK(frozen_at < 480);
		CHECK(w.frozen(7));
		CHECK(worst_gap < 0.05f);
		REQUIRE(w.pose(7, pose));
		const RagdollBodyPose &hips = pose[size_t(def->find_body("Hips"))];
		std::printf("sim.ragdoll: frozen on step %d (%.2f s), hips %.3f m high, worst joint gap %.4f m\n", frozen_at,
				double(frozen_at) / 60.0, double(hips.position.y), double(worst_gap));
		CHECK(hips.position.y < 0.4f);
		CHECK(hips.position.y > 0.05f);
		for (const RagdollBodyPose &b : pose) {
			CHECK(std::isfinite(b.position.x));
			CHECK(b.position.y > -0.05f);
		}
		std::vector<RagdollBodyPose> later(def->bodies.size());
		REQUIRE(w.step(dt).ok());
		REQUIRE(w.pose(7, later));
		CHECK(later[0].position == pose[0].position);
		w.remove(7);
		CHECK_FALSE(w.has(7));
		CHECK(w.ragdoll_count() == 0);
	}

	TEST_CASE("ragdoll world refuses a pose of the wrong size and a non-rotation") {
		std::shared_ptr<const core::res::RagdollDef> def = load_ragdoll();
		if (def == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<RagdollWorld>> world = RagdollWorld::create(floor_world(), def);
		REQUIRE(world.ok());
		std::vector<RagdollBodyPose> pose = rest_pose(*def, 0.0f);
		CHECK_FALSE((*world)->add(1, std::span(pose).subspan(1), {}, {}).ok());
		pose[3].rotation = { 0.0f, 0.0f, 0.0f, 0.0f };
		CHECK_FALSE((*world)->add(1, pose, {}, {}).ok());
		CHECK_FALSE((*world)->step(-1.0f).ok());
		CHECK((*world)->ragdoll_count() == 0);
	}
}
