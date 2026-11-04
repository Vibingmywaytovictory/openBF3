#include <doctest/doctest.h>

#include "bf3/anim/hand_reach.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/viewmodel_anim.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace bf3::anim;

namespace {

Quat turn_deg(float p_x, float p_y, float p_z, float p_deg) {
	return quat_axis_angle(Vec3{ p_x, p_y, p_z }, p_deg * 3.14159265f / 180.0f);
}

struct ArmRig {
	Skeleton rig;
	HandReach reach;
	int32_t wep = -1, ik = -1, hand = -1, arm = -1;

	explicit ArmRig(const Vec3 &p_ik_offset = Vec3{}) {
		std::vector<Bone> b(9);
		b[0].name = "Root";
		b[1].name = "Shoulder";
		b[1].parent = 0;
		b[1].rest = Xform{ Quat{}, Vec3{ 0.2f, 0.0f, 0.0f } };
		b[2].name = "LeftArm";
		b[2].parent = 1;
		b[2].rest = Xform{ turn_deg(0, 1, 0, -30), Vec3{ 0.1f, 0.0f, 0.0f } };
		b[3].name = "LeftArmRoll";
		b[3].parent = 2;
		b[3].rest = Xform{ Quat{}, Vec3{ 0.12f, 0.0f, 0.0f } };
		b[4].name = "LeftForeArm";
		b[4].parent = 3;
		b[4].rest = Xform{ turn_deg(0, 1, 0, 60), Vec3{ 0.18f, 0.0f, 0.0f } };
		b[5].name = "LeftForeArmRoll";
		b[5].parent = 4;
		b[5].rest = Xform{ Quat{}, Vec3{ 0.08f, 0.0f, 0.0f } };
		b[6].name = "LeftHand";
		b[6].parent = 5;
		b[6].rest = Xform{ Quat{}, Vec3{ 0.18f, 0.0f, 0.0f } };
		b[7].name = "Wep_Root";
		b[7].parent = 0;
		b[7].rest = Xform{ Quat{}, Vec3{ 0.0f, -0.1f, 0.4f } };
		b[8].name = "IK_Joint_LeftHand";
		b[8].parent = 7;
		rig = *Skeleton::make(b);
		std::vector<Xform> m;
		model_pose(rig, rest_pose(rig), m);
		b[8].rest = xform_mul(xform_inverse(m[7]), Xform{ m[6].q, vec_add(m[6].t, p_ik_offset) });
		rig = *Skeleton::make(std::move(b));
		arm = rig.find("LeftArm");
		hand = rig.find("LeftHand");
		wep = rig.find("Wep_Root");
		ik = rig.find("IK_Joint_LeftHand");
		reach.arms[0] = HandReachArm{ arm, rig.find("LeftForeArm"), hand, ik, 1.0f };
	}

	std::vector<Xform> held() const {
		std::vector<Xform> m;
		model_pose(rig, rest_pose(rig), m);
		return m;
	}

	Xform hand_at_rest() const {
		std::vector<Xform> m;
		model_pose(rig, rest_pose(rig), m);
		return m[size_t(hand)];
	}

	LocalPose with_effector(const Xform &p_model) const {
		LocalPose p = rest_pose(rig);
		std::vector<Xform> m;
		model_pose(rig, p, m);
		p.joints[size_t(ik)] = xform_mul(xform_inverse(m[size_t(wep)]), p_model);
		return p;
	}
};

Xform model_of(const Skeleton &p_rig, const LocalPose &p_pose, int32_t p_joint) {
	std::vector<Xform> m;
	model_pose(p_rig, p_pose, m);
	return m[size_t(p_joint)];
}

}

TEST_SUITE("anim-viewmodel-unit") {
	TEST_CASE("hand reach: the hand follows its effector's move and turn from the held pose, half way at reach 0.5, "
			  "not at reach 0") {
		ArmRig r;
		const Xform rest = r.hand_at_rest();
		const Xform goal{ quat_mul(turn_deg(1, 0, 0, 20), rest.q), vec_add(rest.t, Vec3{ 0.03f, -0.04f, 0.02f }) };
		std::vector<Xform> scratch;
		LocalPose p = r.with_effector(goal);
		reach_hands(r.rig, r.reach, r.held(), p, scratch);
		const Xform got = model_of(r.rig, p, r.hand);
		CHECK(vec_dist(got.t, goal.t) < 1e-5);
		CHECK(quat_angle_deg(got.q, goal.q) < 0.01);
		CHECK(vec_dist(model_of(r.rig, p, r.ik).t, goal.t) < 1e-6);

		HandReach half = r.reach;
		half.arms[0].reach = 0.5f;
		p = r.with_effector(goal);
		reach_hands(r.rig, half, r.held(), p, scratch);
		CHECK(vec_dist(model_of(r.rig, p, r.hand).t, vec_add(rest.t, Vec3{ 0.015f, -0.02f, 0.01f })) < 1e-5);

		HandReach off = r.reach;
		off.arms[0].reach = 0.0f;
		CHECK(!off.any(r.rig.size()));
		p = r.with_effector(goal);
		const LocalPose before = p;
		reach_hands(r.rig, off, r.held(), p, scratch);
		for (size_t j = 0; j < p.joints.size(); ++j) {
			CHECK(quat_angle_deg(p.joints[j].q, before.joints[j].q) < 1e-6);
		}
	}

	TEST_CASE("hand reach: an effector out of reach straightens the arm toward it; the straight arm then bends to a "
			  "near one; hostile input") {
		ArmRig r;
		const Xform rest = r.hand_at_rest();
		const Vec3 shoulder = model_of(r.rig, rest_pose(r.rig), r.arm).t;
		const Vec3 far = vec_add(shoulder, Vec3{ 0.0f, 0.0f, 2.0f });
		std::vector<Xform> scratch;
		LocalPose p = r.with_effector(Xform{ rest.q, far });
		reach_hands(r.rig, r.reach, r.held(), p, scratch);
		const Vec3 got = model_of(r.rig, p, r.hand).t;
		CHECK(vec_dist(got, shoulder) == doctest::Approx(0.12 + 0.18 + 0.08 + 0.18).epsilon(1e-3));
		CHECK(std::fabs(got.x - shoulder.x) < 1e-4f);
		CHECK(std::fabs(got.y - shoulder.y) < 1e-4f);

		const Vec3 near = vec_add(shoulder, Vec3{ 0.0f, 0.0f, 0.4f });
		p.joints[size_t(r.ik)] = xform_mul(xform_inverse(model_of(r.rig, p, r.wep)), Xform{ rest.q, near });
		reach_hands(r.rig, r.reach, r.held(), p, scratch);
		CHECK(vec_dist(model_of(r.rig, p, r.hand).t, near) < 1e-4);

		LocalPose nan = r.with_effector(Xform{ rest.q, Vec3{ std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f } });
		reach_hands(r.rig, r.reach, r.held(), nan, scratch);
		for (const Xform &x : nan.joints) {
			CHECK(std::isfinite(x.q.w));
		}
		HandReach bad = r.reach;
		bad.arms[0].effector = 99;
		CHECK(!bad.any(r.rig.size()));
		LocalPose short_pose;
		reach_hands(r.rig, r.reach, r.held(), short_pose, scratch);
		CHECK(short_pose.joints.empty());
		LocalPose unheld = r.with_effector(Xform{ rest.q, near });
		const LocalPose unheld_before = unheld;
		reach_hands(r.rig, r.reach, {}, unheld, scratch);
		for (size_t j = 0; j < unheld.joints.size(); ++j) {
			CHECK(quat_angle_deg(unheld.joints[j].q, unheld_before.joints[j].q) < 1e-6);
		}
	}

	TEST_CASE("hand reach: a hand off its effector keeps that offset; it is not pulled onto the effector, and it "
			  "follows the effector's move") {
		const Vec3 offset{ 0.05f, 0.02f, -0.03f };
		ArmRig r(offset);
		const Xform rest = r.hand_at_rest();
		CHECK(vec_dist(model_of(r.rig, rest_pose(r.rig), r.ik).t, vec_add(rest.t, offset)) < 1e-6);
		std::vector<Xform> scratch;
		LocalPose p = rest_pose(r.rig);
		reach_hands(r.rig, r.reach, r.held(), p, scratch);
		CHECK(vec_dist(model_of(r.rig, p, r.hand).t, rest.t) < 1e-6);
		CHECK(quat_angle_deg(model_of(r.rig, p, r.hand).q, rest.q) < 0.01);

		const Vec3 move{ -0.02f, 0.03f, 0.01f };
		p = r.with_effector(Xform{ rest.q, vec_add(vec_add(rest.t, offset), move) });
		reach_hands(r.rig, r.reach, r.held(), p, scratch);
		CHECK(vec_dist(model_of(r.rig, p, r.hand).t, vec_add(rest.t, move)) < 1e-5);
		CHECK(vec_dist(model_of(r.rig, p, r.ik).t, vec_add(vec_add(rest.t, offset), move)) < 1e-6);
	}

	TEST_CASE("hand reach in the animator: an additive layer on the arm leaves the hand where it was on its effector; "
			  "standing the reach changes nothing") {
		ArmRig r(Vec3{ 0.04f, -0.03f, 0.02f });
		bf3::core::ant::DecodedClip delta;
		delta.codec = bf3::core::ant::ClipCodec::Raw;
		delta.additive = true;
		delta.num_keys = 2;
		delta.num_quats = 1;
		delta.end_frame = 30;
		delta.key_times = { 0, 30 };
		delta.quats = { turn_deg(0, 0, 1, 10), turn_deg(0, 0, 1, 10) };
		ClipTracks tracks;
		tracks.rotation_bone = { r.arm };
		tracks.fps = 30.0f;
		tracks.end_frame = 30;
		tracks.channels_bound = 1;
		VmLocomotion loco;
		loco.run.clip.clip = &delta;
		loco.run.clip.tracks = &tracks;
		loco.run.end_frame = 30.0f;
		const std::array<VmClipRef, kVmClips> refs{};
		const ViewmodelTuning tuning;
		const Xform rest = r.hand_at_rest();
		for (const float speed : { 0.0f, 4.0f }) {
			for (const bool held : { false, true }) {
				ViewmodelAnimator a(r.rig, refs, tuning);
				a.set_locomotion(loco);
				if (held) {
					a.set_hand_reach(r.reach);
				}
				ViewmodelFrameInput in;
				in.speed = speed;
				LocalPose pose;
				ViewmodelOutput out;
				for (int f = 0; f < 30; ++f) {
					in.time_s += 1.0 / 60.0;
					in.dt = 1.0f / 60.0f;
					a.step(in, {}, pose, out);
				}
				const double off = vec_dist(model_of(r.rig, pose, r.hand).t, rest.t);
				MESSAGE("speed ", speed, (held ? " held" : " free"), ": hand ", off * 1000.0, " mm from its place");
				CHECK((held || speed == 0.0f ? off < 1e-5 : off > 0.01));
			}
		}
	}
}
