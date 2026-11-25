#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/res/havok_ragdoll.h"
#include "bf3/core/res_types.h"
#include "bf3/core/superbundle_mount.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::core;
using namespace bf3::core::res;

TEST_SUITE("res-ragdoll") {
	TEST_CASE("ragdoll01 (the MP soldier's RagdollAsset): 15 bodies of 5 kg on the soldier bones, 12 ragdoll joints "
			  "and 2 limited hinges with "
			  "coinciding pivots") {
		test::Mounted *m = test::shared_mount_or_skip();
		if (m == nullptr) {
			return;
		}
		Result<LevelMount> mount = m->install->mount_superbundle("Win32/Levels/MP_001/MP_001");
		REQUIRE(mount.ok());
		REQUIRE(mount->load_all_bundles().ok());
		auto ebx = EbxLibrary::open(m->install, { "Win32/Levels/MP_001/MP_001" });
		REQUIRE_MESSAGE(ebx.ok(), (ebx.ok() ? std::string() : ebx.error().message));
		Result<gameplay::SoldierDef> soldier = gameplay::read_soldier_def(**ebx);
		REQUIRE_MESSAGE(soldier.ok(), (soldier.ok() ? std::string() : soldier.error().message));
		CHECK(soldier->ragdoll_asset == "animations/settings/ragdolls/ragdoll01");
		CHECK(soldier->time_for_corpse == 15.0f);
		auto res = mount->read_res(soldier->ragdoll_asset);
		REQUIRE_MESSAGE(res.ok(), (res.ok() ? std::string() : res.error().message));
		CHECK(res_type_name(res->first.res_type) == "RagdollResource");
		RagdollDef def;
		const ResError e = RagdollDef::parse(res->second, res->first.res_meta, def);
		REQUIRE_MESSAGE(e.ok(), e.message());
		CHECK(def.packfile_version == "hk_2010.2.0-r1");
		REQUIRE(def.bodies.size() == 15);
		REQUIRE(def.joints.size() == 14);
		const std::set<std::string> bones{ "Hips", "LeftUpLeg", "LeftLeg", "RightUpLeg", "RightLeg", "Spine", "Spine1",
			"Spine2", "Head", "LeftArm", "LeftArmRoll", "LeftForeArm", "RightArm", "RightArmRoll", "RightForeArm" };
		std::set<std::string> seen;
		uint32_t capsules = 0, spheres = 0;
		for (const RagdollBodyDef &b : def.bodies) {
			seen.insert(b.bone);
			CHECK(b.mass == doctest::Approx(5.0f));
			CHECK(b.friction == doctest::Approx(0.5f));
			CHECK(b.radius > 0.05f);
			CHECK(b.radius < 0.15f);
			CHECK(std::fabs(b.rest.determinant() - 1.0f) < 1.0e-3f);
			(b.shape == RagdollShapeKind::Capsule ? capsules : spheres)++;
		}
		CHECK(seen == bones);
		CHECK(capsules == 11);
		CHECK(spheres == 4);
		const RagdollBodyDef &hips = def.bodies[size_t(def.find_body("Hips"))];
		CHECK(hips.rest.trans.y == doctest::Approx(0.9607f).epsilon(1e-3));
		CHECK(hips.angular_damping == doctest::Approx(0.0498f).epsilon(1e-3));
		CHECK(hips.linear_damping == 0.0f);
		uint32_t hinges = 0;
		for (const RagdollJointDef &j : def.joints) {
			CHECK(def.pivot_gap(j) < 1.0e-4f);
			CHECK(j.name == def.bodies[j.child].name);
			if (j.kind == RagdollJointKind::LimitedHinge) {
				++hinges;
				CHECK(def.bodies[j.child].bone.find("ArmRoll") != std::string::npos);
				CHECK(j.hinge_min == doctest::Approx(-0.349066f));
				CHECK(j.hinge_max == doctest::Approx(0.349066f));
			}
		}
		CHECK(hinges == 2);
		const RagdollJointDef &up_leg = def.joints[0];
		CHECK(def.bodies[up_leg.child].bone == "LeftUpLeg");
		CHECK(def.bodies[up_leg.parent].bone == "Hips");
		CHECK(up_leg.twist_min == doctest::Approx(-0.610865f));
		CHECK(up_leg.twist_max == doctest::Approx(0.349066f));
		CHECK(up_leg.cone == doctest::Approx(0.698132f));
		CHECK(up_leg.plane_min == doctest::Approx(-0.261799f));
		CHECK(up_leg.plane_max == doctest::Approx(0.261799f));
		CHECK(up_leg.max_friction_torque == doctest::Approx(2.0f));
	}
}
