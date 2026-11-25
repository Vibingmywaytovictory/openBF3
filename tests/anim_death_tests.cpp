#include <doctest/doctest.h>

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/death_anim.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/res/havok_ragdoll.h"
#include "bf3/core/superbundle_mount.h"
#include "test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bf3::anim;

namespace {

struct DeathData {
	bool ok = false;
	std::string why;
	bf3::core::InstallReader reader;
	std::shared_ptr<bf3::core::Install> install;
	std::unique_ptr<bf3::core::ant::AntInstallSource> source;
	std::unique_ptr<bf3::core::ant::AntLibrary> library;
	CrowdClipSet clips;
	bf3::core::res::RagdollDef ragdoll;
};

DeathData *death_data() {
	static DeathData *data = [] {
		auto *d = new DeathData();
		if (!d->reader.locate({})) {
			d->why = d->reader.describe();
			return d;
		}
		auto install = bf3::core::Install::open(d->reader);
		REQUIRE_MESSAGE(install.ok(), (install.ok() ? "" : install.error().message));
		d->install = *install;
		auto source = bf3::core::ant::AntInstallSource::open(d->install);
		REQUIRE_MESSAGE(source.ok(), (source.ok() ? "" : source.error().message));
		d->source = std::move(*source);
		auto lib = load_ant_library(*d->source);
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? "" : lib.error().message));
		d->library = std::move(*lib);
		auto clips = load_crowd_clips(*d->source, *d->library);
		REQUIRE_MESSAGE(clips.ok(), (clips.ok() ? "" : clips.error().message));
		d->clips = std::move(*clips);
		auto mount = d->install->mount_superbundle("Win32/Levels/MP_001/MP_001");
		REQUIRE(mount.ok());
		REQUIRE(mount->load_all_bundles().ok());
		auto res = mount->read_res("animations/settings/ragdolls/ragdoll01");
		REQUIRE(res.ok());
		const bf3::core::res::ResError e =
				bf3::core::res::RagdollDef::parse(res->second, res->first.res_meta, d->ragdoll);
		REQUIRE_MESSAGE(e.ok(), e.message());
		d->ok = true;
		return d;
	}();
	return data;
}

DeathData *death_or_skip() {
	DeathData *d = death_data();
	if (!d->ok) {
		bf3::test::skip_install_case(d->why.c_str());
		return nullptr;
	}
	return d;
}

}

TEST_SUITE("anim-death") {
	TEST_CASE("death flow: DeathNode tries explosion, prone, crouch, then stand; prone plays Death_Prone1, crouch "
			  "picks by RandomValue") {
		DeathData *d = death_or_skip();
		if (d == nullptr) {
			return;
		}
		bf3::core::Result<AntDeathFlow> flow = read_death_flow(*d->library, ChooserIndices{});
		REQUIRE_MESSAGE(flow.ok(), (flow.ok() ? "" : flow.error().message));
		REQUIRE(flow->nodes.size() == 4);
		CHECK(flow->nodes[0].name == "General_ExplosionDeathNode");
		CHECK(flow->nodes[0].requires_true == std::vector<std::string>{ std::string(kDeathExplosionSignal) });
		CHECK(flow->nodes[1].name == "General_ProneDeathNode");
		CHECK(flow->nodes[2].name == "General_CrouchDeathNode");
		CHECK(flow->nodes[3].name == "General_StandDeathNode");
		CHECK(flow->nodes[3].requires_true.empty());
		CHECK(flow->nodes[3].subject == "ContextDatabaseChooserWrapperAsset 'MP_StandStance_ContextDatabaseWrapper'");
		CHECK(flow->nodes[0].subject ==
				"ContextDatabaseChooserWrapperAsset 'MP_ExplosionDeath_ContextDatabaseWrapper'");
		CHECK(flow->prone.controller == "Death_Prone1");
		CHECK(flow->crouch_signal == kDeathRandomValueSignal);
		std::vector<float> thresholds;
		std::vector<std::string> controllers;
		for (const AntLinearPick &p : flow->crouch) {
			thresholds.push_back(p.threshold);
			controllers.push_back(p.route.controller);
		}
		CHECK(thresholds == std::vector<float>{ 0.0f, 0.17f, 0.33f, 0.48f, 0.66f, 0.83f, 1.0f });
		CHECK(controllers ==
				std::vector<std::string>{ "Death_Crouch_Bwd", "Death_Crouch_Front_Body2", "Death_Crouch_Front_Body4",
						"Death_Crouch_Front_Body", "Death_Crouch_Fwd", "Death_Crouch_Right_Body", "Death_Crouch1" });
		CHECK(linear_pick(flow->crouch, 0.5f)->route.controller == "Death_Crouch_Front_Body");
		CHECK(linear_pick(flow->crouch, 1.0f)->route.controller == "Death_Crouch1");
		CHECK(linear_pick(flow->crouch, 1.0f / 16777216.0f)->route.controller == "Death_Crouch_Bwd");
		const AntLinearPick &last = flow->crouch.back();
		REQUIRE(last.route.float_tags.size() >= 1);
		bool found = false;
		for (const AntFloatStateTag &t : last.route.float_tags) {
			if (t.signal == kRagdollBlendSignal) {
				found = true;
				CHECK(t.tick == 39.0f);
				CHECK(t.ticks == 15.0f);
				CHECK(t.spline.size() == 16);
				CHECK(t.at(38.0f) == 0.0f);
				CHECK(t.at(54.0f) == 1.0f);
				CHECK(t.at(46.5f) > 0.0f);
				CHECK(t.at(46.5f) < 1.0f);
			}
		}
		CHECK(found);
	}

	TEST_CASE("death clips: the choice per node, the clips on the rig and the RagdollBlend target over time") {
		DeathData *d = death_or_skip();
		if (d == nullptr) {
			return;
		}
		bf3::core::Result<CrowdDeathClips> clips = load_crowd_death_clips(d->clips, *d->library, ChooserIndices{});
		REQUIRE_MESSAGE(clips.ok(), (clips.ok() ? "" : clips.error().message));
		CHECK(clips->clips.size() == 8);
		CHECK(clips->order ==
				std::vector<CrowdDeathNode>{ CrowdDeathNode::Explosion, CrowdDeathNode::Prone, CrowdDeathNode::Crouch,
						CrowdDeathNode::Stand });
		CrowdDeathChoice c;
		CHECK(clips->node(c) == CrowdDeathNode::Stand);
		CHECK(clips->choose(c) == kNoCrowdDeathClip);
		c.prone = true;
		c.explosion = true;
		CHECK(clips->node(c) == CrowdDeathNode::Explosion);
		CHECK(clips->choose(c) == kNoCrowdDeathClip);
		c.explosion = false;
		REQUIRE(clips->choose(c) == clips->prone);
		const CrowdDeathClip &prone = clips->clips[clips->prone];
		CHECK(prone.controller == "Death_Prone1");
		CHECK(prone.ragdoll_target(0.0f) == 0.0f);
		CHECK(prone.ragdoll_target(15.0f / 60.0f) == 1.0f);
		c.prone = false;
		c.crouch = true;
		c.random_value = 0.5f;
		CHECK(clips->clips[clips->choose(c)].controller == "Death_Crouch_Front_Body");
		c.random_value = 1.0f;
		const CrowdDeathClip &crouch1 = clips->clips[clips->choose(c)];
		CHECK(crouch1.controller == "Death_Crouch1");
		CHECK(crouch1.ragdoll_target(38.0f / 60.0f) == 0.0f);
		CHECK(crouch1.ragdoll_target(54.0f / 60.0f) == 1.0f);
		CHECK(crouch1.play.seconds() > 54.0f / 60.0f);
		for (const CrowdDeathClip &k : clips->clips) {
			CHECK(k.clip.mask.size() == d->clips.rig.size());
			CHECK(k.ragdoll.size() == 1);
		}
	}

	TEST_CASE("ragdoll rig: each ragdoll01 body's joint pivot is its rig joint, trunk and legs share the joint's rest "
			  "rotation, and bodies and joints convert both ways") {
		DeathData *d = death_or_skip();
		if (d == nullptr) {
			return;
		}
		const bf3::core::res::RagdollDef &rd = d->ragdoll;
		std::vector<std::string> bones;
		std::vector<Vec3> pivots(rd.bodies.size(), Vec3{});
		for (const bf3::core::res::RagdollBodyDef &b : rd.bodies) {
			bones.push_back(b.bone);
		}
		for (const bf3::core::res::RagdollJointDef &j : rd.joints) {
			pivots[j.child] = j.child_frame.trans;
		}
		bf3::core::Result<RagdollRig> rig = RagdollRig::make(d->clips.rig, bones, pivots);
		REQUIRE_MESSAGE(rig.ok(), (rig.ok() ? "" : rig.error().message));
		std::vector<Xform> model;
		const LocalPose pose = rest_pose(d->clips.rig);
		model_pose(d->clips.rig, pose, model);
		double worst_pivot = 0.0, worst_angle = 0.0;
		for (size_t b = 0; b < bones.size(); ++b) {
			const bool lower_arm =
					bones[b].find("ArmRoll") != std::string::npos || bones[b].find("ForeArm") != std::string::npos;
			const Xform body{ Quat{ rd.bodies[b].rotation[0], rd.bodies[b].rotation[1], rd.bodies[b].rotation[2],
									  rd.bodies[b].rotation[3] },
				rd.bodies[b].rest.trans };
			const Vec3 pivot = xform_point(body, pivots[b]);
			const Xform &joint = model[size_t(rig->joints[b])];
			if (!lower_arm) {
				worst_pivot = std::max(worst_pivot, vec_dist(pivot, joint.t));
			}
			if (bones[b].find("Arm") == std::string::npos) {
				worst_angle = std::max(worst_angle, quat_angle_deg(body.q, joint.q));
			}
		}
		std::printf("anim.death: pivot to rig joint at rest at most %.4f m, trunk and leg body rotation at most %.3f "
					"deg off the joint\n",
				worst_pivot, worst_angle);
		CHECK(worst_pivot < 0.015);
		CHECK(worst_angle < 1.0);
		const Xform root{ quat_axis_angle(Vec3{ 0.0f, 1.0f, 0.0f }, 1.0f), Vec3{ 10.0f, 2.0f, -3.0f } };
		std::vector<Xform> bodies(bones.size()), joints(bones.size());
		rig->bodies(model, root, bodies);
		rig->joints_model(bodies, root, joints);
		for (size_t b = 0; b < bones.size(); ++b) {
			CHECK(vec_dist(joints[b].t, model[size_t(rig->joints[b])].t) < 1.0e-4);
			CHECK(quat_angle_deg(joints[b].q, model[size_t(rig->joints[b])].q) < 0.05);
		}
		LocalPose out;
		ragdoll_local_pose(d->clips.rig, pose, rig->joints, joints, out);
		std::vector<Xform> again;
		model_pose(d->clips.rig, out, again);
		for (size_t j = 0; j < again.size(); ++j) {
			CHECK(vec_dist(again[j].t, model[j].t) < 1.0e-4);
		}
	}
}
