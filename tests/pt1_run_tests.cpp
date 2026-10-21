#include <doctest/doctest.h>

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/locomotion.h"
#include "bf3/anim/move_group.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/render/viewmodel_set.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace bf3::anim;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

MoveGroupDef synth_run_group() {
	MoveGroupDef d;
	d.name = "synthetic run";
	d.x_min = -kPi;
	d.x_max = 1.5f * kPi;
	d.one_to_one_speed = 1.0f;
	d.min_speed = 0.01f;
	d.master_length = 100.0f;
	for (const char *n : { "zero", "fwd", "bwd", "left", "right" }) {
		AntClipRoute r;
		r.controller = n;
		d.clips.push_back(r);
	}
	auto dir = [](float p_x, uint32_t p_clip, float p_v, float p_speed) {
		MoveGroupDirection m;
		m.x = p_x;
		m.entries.push_back(MoveGroupEntry{ 0.0f, 1.0f, 0.0f, 0.0f, 0 });
		m.entries.push_back(MoveGroupEntry{ p_v, p_speed, 0.0f, 0.0f, p_clip });
		return m;
	};
	d.directions = { dir(-kPi, 2, 0.0494f, 0.999302f), dir(-0.5f * kPi, 3, 0.0433256f, 1.0f), dir(0.0f, 1, 0.0705144f, 1.0f),
		dir(0.5f * kPi, 4, 0.0426388f, 0.911911f), dir(kPi, 2, 0.0494f, 0.999302f), dir(1.5f * kPi, 3, 0.0433256f, 1.0f) };
	return d;
}

std::vector<MoveGroupClipTiming> synth_run_timing() {
	return { { 0.0f, 0.0f }, { 0.6f, 4.64f / 60.0f }, { 0.5667f, 2.975f / 60.0f }, { 0.5667f, 2.598f / 60.0f }, { 0.5333f, 3.206f / 60.0f } };
}

float weight_of(const MoveGroupWeights &p_w, uint32_t p_clip) {
	for (uint32_t k = 0; k < p_w.count; ++k) {
		if (p_w.terms[k].clip == p_clip) {
			return p_w.terms[k].weight;
		}
	}
	return 0.0f;
}

float rate_of(const MoveGroupWeights &p_w, uint32_t p_clip) {
	for (uint32_t k = 0; k < p_w.count; ++k) {
		if (p_w.terms[k].clip == p_clip) {
			return p_w.terms[k].rate;
		}
	}
	return 0.0f;
}

float stride_speed(const MoveGroupWeights &p_w, std::span<const MoveGroupClipTiming> p_timing) {
	float s = 0.0f;
	for (uint32_t k = 0; k < p_w.count; ++k) {
		const MoveGroupTerm &t = p_w.terms[k];
		s += t.weight * t.rate * p_timing[t.clip].root_velocity * kAntTicksPerSecond;
	}
	return s;
}

Quat axis_angle(float x, float y, float z, float p_deg) {
	const float h = p_deg * kPi / 360.0f;
	const float s = std::sin(h);
	return Quat{ x * s, y * s, z * s, std::cos(h) };
}

Skeleton synth_rig() {
	std::vector<Bone> bones(4);
	bones[0].name = "AITrajectory";
	bones[1].name = "Hips";
	bones[1].parent = 0;
	bones[1].rest = Xform{ Quat{}, Vec3{ 0, 1, 0 } };
	bones[2].name = "Arm";
	bones[2].parent = 1;
	bones[2].rest = Xform{ Quat{}, Vec3{ 0.2f, 0.4f, 0 } };
	bones[3].name = "Wep";
	bones[3].parent = 2;
	bones[3].rest = Xform{ Quat{}, Vec3{ 0.3f, 0, 0 } };
	return *Skeleton::make(std::move(bones));
}

CrowdClipData const_clip(const Skeleton &p_rig, uint32_t p_end_frame, std::vector<int32_t> p_bones, const Quat &p_q) {
	CrowdClipData d;
	bf3::core::ant::DecodedClip &c = d.clip;
	c.codec = bf3::core::ant::ClipCodec::Raw;
	c.num_keys = 2;
	c.num_quats = uint32_t(p_bones.size());
	c.num_vec3s = 0;
	c.end_frame = p_end_frame;
	c.key_times = { 0, uint16_t(p_end_frame) };
	for (uint32_t k = 0; k < 2; ++k) {
		for (size_t q = 0; q < p_bones.size(); ++q) {
			c.quats.push_back(p_q);
		}
	}
	d.tracks.rotation_bone = std::move(p_bones);
	d.tracks.fps = 30.0f;
	d.tracks.end_frame = p_end_frame;
	d.mask = mask_from_tracks(p_rig, d.tracks);
	return d;
}

CrowdMoveGroup synth_group(const Skeleton &p_rig, float p_velocity, float p_item_speed, const Quat &p_zero, const Quat &p_move, float p_min_speed) {
	CrowdMoveGroup g;
	g.ok = true;
	g.x_sign = -1.0f;
	g.def.x_min = -kPi;
	g.def.x_max = kPi;
	g.def.one_to_one_speed = 1.0f;
	g.def.min_speed = p_min_speed;
	g.def.clips.resize(2);
	MoveGroupDirection d;
	d.entries = { MoveGroupEntry{ 0.0f, 1.0f, 0.0f, 0.0f, 0 }, MoveGroupEntry{ p_velocity, p_item_speed, 0.0f, 0.0f, 1 } };
	g.def.directions = { d };
	g.clips.push_back(const_clip(p_rig, 1, { 2 }, p_zero));
	g.clips.push_back(const_clip(p_rig, 18, { 2 }, p_move));
	g.timing = { { 0.0f, 0.0f }, { 0.6f, 0.0f } };
	g.root_dir = { Vec3{}, Vec3{ 0, 0, 1 } };
	return g;
}

struct SynthPt1 {
	CrowdClipSet set;
	SkinBinds binds;
	std::vector<CrowdTeam> teams;
	CrowdMoveGroupSet groups;
	Quat run_q = axis_angle(1, 0, 0, 30), sprint_q = axis_angle(1, 0, 0, -40), sneak_q = axis_angle(0, 0, 1, 50), zero_q = axis_angle(0, 0, 1, -10);

	SynthPt1() {
		set.rig = synth_rig();
		set.root_joint = 0;
		const Skeleton &r = set.rig;
		set.clips[size_t(CrowdClip::Idle)] = const_clip(r, 1, { 1, 2 }, axis_angle(0, 1, 0, 5));
		for (CrowdClip c : { CrowdClip::RunFwd, CrowdClip::RunBwd, CrowdClip::RunLeft, CrowdClip::RunRight }) {
			set.clips[size_t(c)] = const_clip(r, 18, { 2 }, axis_angle(0, 1, 0, 70));
		}
		set.clips[size_t(CrowdClip::AimUp)] = const_clip(r, 1, { 2 }, Quat{});
		set.clips[size_t(CrowdClip::AimDown)] = const_clip(r, 1, { 2 }, Quat{});
		set.clips[size_t(CrowdClip::AimUp)].additive = set.clips[size_t(CrowdClip::AimDown)].additive = true;
		set.loco[size_t(LocoClip::Fwd)] = LocoClipInfo{ 0.6f, 4.64f, Vec3{ 0, 0, 1 } };
		set.loco[size_t(LocoClip::Bwd)] = LocoClipInfo{ 0.567f, 2.97f, Vec3{ 0, 0, -1 } };
		set.loco[size_t(LocoClip::Left)] = LocoClipInfo{ 0.567f, 2.6f, Vec3{ 1, 0, 0 } };
		set.loco[size_t(LocoClip::Right)] = LocoClipInfo{ 0.533f, 3.2f, Vec3{ -1, 0, 0 } };
		EbxSkeleton ebx;
		ebx.partition = "synthetic";
		std::vector<Xform> model;
		model_pose(r, rest_pose(r), model);
		for (size_t i = 0; i < r.size(); ++i) {
			ebx.bone_names.push_back(r.bone(i).name);
			ebx.parents.push_back(r.bone(i).parent);
			ebx.local_pose.push_back(mat_from_xform(r.bone(i).rest));
			ebx.model_pose.push_back(mat_from_xform(model[i]));
		}
		for (uint32_t i = 0; i < r.size(); ++i) {
			REQUIRE(binds.bind(ebx, i, r).ok());
		}
		CrowdTeam t;
		t.binds = &binds;
		teams = { t, t };
		groups.groups[size_t(CrowdMoveGroupRole::Run)] = synth_group(r, 0.0705f, 1.0f, zero_q, run_q, 0.01f);
		groups.groups[size_t(CrowdMoveGroupRole::Sprint)] = synth_group(r, 0.096f, 1.11f, zero_q, sprint_q, 0.0f);
		groups.groups[size_t(CrowdMoveGroupRole::Sneak)] = synth_group(r, 0.033f, 1.909f, zero_q, sneak_q, 0.01f);
		groups.options.velocity_from_root = true;
	}
};

Quat arm_local(const CrowdEvaluator &p_e) {
	const std::span<const Xform> m = p_e.model(0);
	REQUIRE(m.size() == 4);
	return quat_mul(quat_conj(m[1].q), m[2].q);
}

}

TEST_SUITE("pt1-run-unit") {
	TEST_CASE("move group weights: clip choice per direction and speed") {
		const MoveGroupDef d = synth_run_group();
		const std::vector<MoveGroupClipTiming> tm = synth_run_timing();
		const MoveGroupOptions data{ false }, root{ true };

		MoveGroupWeights w = move_group_weights(d, tm, 0.0f, 4.0f, data);
		CHECK(w.count == 2);
		CHECK(weight_of(w, 1) == doctest::Approx(4.0 / 60.0 / 0.0705144).epsilon(1e-4));
		CHECK(weight_of(w, 0) == doctest::Approx(1.0 - 4.0 / 60.0 / 0.0705144).epsilon(1e-3));
		CHECK(rate_of(w, 1) == doctest::Approx(1.0));
		CHECK(w.cycles_per_second == doctest::Approx(1.0 / 0.6).epsilon(1e-4));
		w = move_group_weights(d, tm, 0.0f, 4.0f, root);
		CHECK(weight_of(w, 1) == doctest::Approx(4.0 / 4.64).epsilon(1e-4));
		CHECK(w.cycles_per_second == doctest::Approx(1.0 / 0.6).epsilon(1e-4));

		struct Case {
			float x;
			uint32_t clip;
		} cases[] = { { 0.0f, 1 }, { -0.5f * kPi, 3 }, { 0.5f * kPi, 4 }, { kPi, 2 }, { -kPi, 2 } };
		for (const Case &c : cases) {
			w = move_group_weights(d, tm, c.x, 2.5f, root);
			CHECK(w.count == 2);
			CHECK(weight_of(w, c.clip) > 0.5f);
			CHECK(weight_of(w, c.clip) + weight_of(w, 0) == doctest::Approx(1.0));
			CHECK(w.moving == 1);
		}

		w = move_group_weights(d, tm, -0.25f * kPi, 2.0f, root);
		CHECK(w.count == 3);
		CHECK(weight_of(w, 1) + weight_of(w, 3) + weight_of(w, 0) == doctest::Approx(1.0));
		CHECK(weight_of(w, 1) == doctest::Approx(0.5 * 2.0 / 4.64).epsilon(1e-3));
		CHECK(weight_of(w, 3) == doctest::Approx(0.5 * 2.0 / 2.598).epsilon(1e-3));

		w = move_group_weights(d, tm, 4.0f, 2.0f, root);
		CHECK(weight_of(w, 2) > 0.0f);
		CHECK(weight_of(w, 3) > 0.0f);
		CHECK(w.x == doctest::Approx(4.0 - 2.0 * kPi).epsilon(1e-5));
		w = move_group_weights(d, tm, -4.0f, 2.0f, root);
		CHECK(w.x == doctest::Approx(-4.0 + 2.0 * kPi).epsilon(1e-5));
		CHECK(weight_of(w, 4) > 0.0f);
		CHECK(weight_of(w, 2) > 0.0f);

		w = move_group_weights(d, tm, 0.0f, 0.0f, root);
		CHECK(w.count == 1);
		CHECK(weight_of(w, 0) == doctest::Approx(1.0));
		CHECK(w.below_min_speed);
		CHECK(w.cycles_per_second == 0.0f);
		CHECK(move_group_weights(d, tm, 0.0f, 0.59f, root).below_min_speed);
		CHECK_FALSE(move_group_weights(d, tm, 0.0f, 0.61f, root).below_min_speed);
	}

	TEST_CASE("move group weights: cycle rate per speed, one to one above the range") {
		const MoveGroupDef d = synth_run_group();
		const std::vector<MoveGroupClipTiming> tm = synth_run_timing();
		const MoveGroupOptions root{ true };

		for (float speed : { 1.0f, 2.0f, 3.0f, 4.0f, 4.64f }) {
			const MoveGroupWeights w = move_group_weights(d, tm, 0.0f, speed, root);
			CHECK(rate_of(w, 1) == doctest::Approx(1.0));
			CHECK(w.cycles_per_second == doctest::Approx(1.0 / 0.6).epsilon(1e-4));
		}
		for (float speed : { 5.0f, 6.5f, 9.28f }) {
			const MoveGroupWeights w = move_group_weights(d, tm, 0.0f, speed, root);
			CHECK(w.count == 1);
			CHECK(rate_of(w, 1) == doctest::Approx(speed / 4.64).epsilon(1e-4));
			CHECK(w.cycles_per_second == doctest::Approx(speed / 4.64 / 0.6).epsilon(1e-4));
		}

		const MoveGroupWeights r = move_group_weights(d, tm, 0.5f * kPi, 2.0f, root);
		CHECK(rate_of(r, 4) == doctest::Approx(0.911911));
		CHECK(r.cycles_per_second == doctest::Approx(0.911911 / 0.5333).epsilon(1e-3));
	}

	TEST_CASE("move group weights: the strides give the ground speed (no foot sliding) with velocity_from_root") {
		const MoveGroupDef d = synth_run_group();
		const std::vector<MoveGroupClipTiming> tm = synth_run_timing();
		const MoveGroupOptions root{ true }, data{ false };
		float worst = 0.0f, worst_data = 0.0f;
		for (float speed = 0.25f; speed <= 9.0f; speed += 0.25f) {
			for (float x = -kPi; x <= 1.5f * kPi + 1e-3f; x += kPi / 8.0f) {
				const MoveGroupWeights w = move_group_weights(d, tm, x, speed, root);
				worst = std::max(worst, std::fabs(stride_speed(w, tm) / speed - 1.0f));
				const MoveGroupWeights wd = move_group_weights(d, tm, x, speed, data);
				worst_data = std::max(worst_data, std::fabs(stride_speed(wd, tm) / speed - 1.0f));
			}
		}
		MESSAGE("worst |stride / ground - 1|: root velocities " << worst << ", the data's Velocity " << worst_data);
		CHECK(worst < 1e-4f);
		CHECK(worst_data > 0.05f);
	}

	TEST_CASE("move group weights: hostile input") {
		const MoveGroupDef d = synth_run_group();
		const std::vector<MoveGroupClipTiming> tm = synth_run_timing();
		const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
		for (float x : { nan, inf, -inf, 1.0e30f, -1.0e30f }) {
			for (float speed : { nan, inf, -inf, -5.0f, 0.0f, 1.0e30f, 4.0f }) {
				const MoveGroupWeights w = move_group_weights(d, tm, x, speed);
				CHECK(w.count >= 1);
				CHECK(w.count <= kMaxMoveGroupTerms);
				float sum = 0.0f;
				for (uint32_t k = 0; k < w.count; ++k) {
					CHECK(std::isfinite(w.terms[k].rate));
					CHECK(w.terms[k].clip < d.clips.size());
					sum += w.terms[k].weight;
				}
				CHECK(sum == doctest::Approx(1.0));
				CHECK(std::isfinite(w.cycles_per_second));
			}
		}
		CHECK(move_group_weights(MoveGroupDef{}, tm, 0.0f, 4.0f).count == 0);
		CHECK(move_group_weights(d, {}, 0.0f, 4.0f).cycles_per_second == 0.0f);
		CHECK(move_group_x(Vec3{ nan, 0, 1 }, -1.0f) == 0.0f);
		CHECK(move_group_x(Vec3{}, -1.0f) == 0.0f);
		CHECK(move_group_x(Vec3{ 1, 0, 0 }, -1.0f) == doctest::Approx(-0.5 * kPi));
		CHECK(move_group_x(Vec3{ 0, 0, -1 }, 1.0f) == doctest::Approx(kPi));
	}

	TEST_CASE("move group weights: hostile def (huge or non-finite Min / Max, root order against the data's)") {
		const std::vector<MoveGroupClipTiming> tm = synth_run_timing();
		const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();

		const std::pair<float, float> ranges[] = { { 1.0e9f, 1.0e9f + 1.0f }, { -1.0e9f, -1.0e9f + 64.0f }, { 1.0e6f, 1.0e6f + 3.0f },
			{ -3.0e38f, 3.0e38f }, { nan, 1.0f }, { 0.0f, inf }, { 2.0f, 1.0f } };
		for (const auto &[lo, hi] : ranges) {
			MoveGroupDef d = synth_run_group();
			d.x_min = lo;
			d.x_max = hi;
			for (float x : { 0.0f, -kPi, 2.0f, 1.0e4f, nan }) {
				const MoveGroupWeights w = move_group_weights(d, tm, x, 4.0f);
				CHECK(std::isfinite(w.x));
				CHECK(w.count >= 1);
				float sum = 0.0f;
				for (uint32_t k = 0; k < w.count; ++k) {
					sum += w.terms[k].weight;
				}
				CHECK(sum == doctest::Approx(1.0));
			}
		}

		const MoveGroupDef d = synth_run_group();
		CHECK(move_group_weights(d, tm, -kPi + 0.5f - 2.0f * kPi, 4.0f).x == doctest::Approx(-kPi + 0.5f).epsilon(1e-4));
		CHECK(move_group_weights(d, tm, 0.25f + 6.0f * kPi, 4.0f).x == doctest::Approx(0.25).epsilon(1e-4));

		MoveGroupDef r = synth_run_group();
		for (MoveGroupDirection &dir : r.directions) {
			dir.entries = { MoveGroupEntry{ 0.02f, 1.0f, 0.0f, 0.0f, 1 }, MoveGroupEntry{ 0.04f, 1.0f, 0.0f, 0.0f, 2 },
				MoveGroupEntry{ 0.06f, 1.0f, 0.0f, 0.0f, 3 } };
		}
		std::vector<MoveGroupClipTiming> rt = tm;
		rt[1].root_velocity = 6.0f / 60.0f;
		rt[2].root_velocity = 2.0f / 60.0f;
		rt[3].root_velocity = 4.0f / 60.0f;
		for (float speed : { 1.0f, 3.0f, 3.5f, 4.0f, 5.0f, 8.0f }) {
			const MoveGroupWeights w = move_group_weights(r, rt, 0.0f, speed, MoveGroupOptions{ true });
			float sum = 0.0f;
			for (uint32_t k = 0; k < w.count; ++k) {
				sum += w.terms[k].weight;
			}
			CHECK(sum == doctest::Approx(1.0));
		}
	}

	TEST_CASE("crowd evaluator with move groups: run, sprint and its ramp, crouch sneak, prone unchanged, idle below MinSpeed") {
		SynthPt1 s;
		CrowdStanceSet stance;
		stance.move_groups = &s.groups;
		CrowdConfig cfg;
		cfg.temporal_lod = false;
		auto e = CrowdEvaluator::make(s.set, stance, s.teams, 1, cfg);
		REQUIRE(e.ok());
		CrowdEvaluator &ev = **e;
		CrowdSoldierInput in;
		in.velocity = Vec3{ 0, 0, 4.0f };
		auto step = [&](double p_dt) {
			ev.set_inputs(std::span<const CrowdSoldierInput>(&in, 1), p_dt);
			ev.evaluate_all_here();
			return arm_local(ev);
		};

		Quat q = step(1.0 / 60.0);
		Quat want{};
		{
			LocalPose a, b, o;
			a.joints = { Xform{ s.zero_q, {} } };
			b.joints = { Xform{ s.run_q, {} } };
			blend_poses(a, b, 4.0f / 60.0f / 0.0705f, o);
			want = o.joints[0].q;
		}
		CHECK(quat_angle_deg(q, want) < 0.05);
		CHECK(ev.combat_stats().stand == 1);

		in.velocity = Vec3{ 0, 0, 6.5f };
		in.sprinting = true;
		q = step(0.5 * kCrowdSprintBlendSeconds);
		const double total = quat_angle_deg(s.run_q, s.sprint_q);
		const double from_sprint = quat_angle_deg(q, s.sprint_q);
		MESSAGE("halfway into the sprint ramp: " << from_sprint << " deg from the sprint pose of " << total);
		CHECK(from_sprint == doctest::Approx(0.5 * total).epsilon(0.05));
		q = step(0.5 * kCrowdSprintBlendSeconds + 1e-3);
		CHECK(quat_angle_deg(q, s.sprint_q) < 0.05);

		in.sprinting = false;
		in.velocity = Vec3{ 0, 0, 4.23f };
		step(kCrowdSprintBlendSeconds + 1e-3);
		q = step(1.0 / 60.0);
		CHECK(quat_angle_deg(q, s.run_q) < 0.05);

		in.velocity = Vec3{};
		step(kCrowdMoveBlendSeconds + 1e-3);
		q = step(1.0 / 60.0);
		CHECK(quat_angle_deg(q, axis_angle(0, 1, 0, 5)) < 0.05);
		CHECK_FALSE(ev.combat_stats().moving_crouch);

		in.pose = in.target_pose = 1;
		in.velocity = Vec3{ 0, 0, 2.0f };
		q = step(1.0 / 60.0);
		CHECK(quat_angle_deg(q, s.sneak_q) < 0.05);
		CHECK(ev.combat_stats().moving_crouch == 1);

		in.sprinting = true;
		q = step(1.0 / 60.0);
		CHECK(quat_angle_deg(q, s.sneak_q) < 0.05);

		in.pose = in.target_pose = 2;
		in.sprinting = false;
		in.velocity = Vec3{ 0, 0, 0.9f };
		q = step(1.0 / 60.0);
		CHECK(quat_angle_deg(q, s.sneak_q) > 10.0);
		CHECK(quat_angle_deg(q, s.run_q) > 10.0);

		in.pose = in.target_pose = 0;
		in.velocity = Vec3{ std::numeric_limits<float>::quiet_NaN(), 0, 0 };
		q = step(1.0 / 60.0);
		CHECK(std::isfinite(q.w));
		s.groups.groups[size_t(CrowdMoveGroupRole::Run)].ok = false;
		in.velocity = Vec3{ 0, 0, 4.0f };
		q = step(1.0 / 60.0);
		CHECK(quat_angle_deg(q, axis_angle(0, 1, 0, 70)) < 0.5);
	}
}

namespace {

struct Pt1Data {
	bool ok = false;
	std::string why;
	bf3::core::InstallReader reader;
	bf3::core::InstallSnapshot before;
	std::shared_ptr<bf3::core::Install> install;
	std::unique_ptr<bf3::core::ant::AntInstallSource> source;
	std::unique_ptr<bf3::core::ant::AntLibrary> library;
	CrowdClipSet clips;
	CrowdMoveClips moves;
	CrowdMoveGroupSet groups;
	double seconds_library = 0, seconds_clips = 0, seconds_groups = 0;
};

Pt1Data *pt1_data() {
	static Pt1Data *data = [] {
		auto *d = new Pt1Data();
		if (!d->reader.locate({})) {
			d->why = d->reader.describe();
			return d;
		}
		d->before = d->reader.snapshot();
		auto install = bf3::core::Install::open(d->reader);
		REQUIRE_MESSAGE(install.ok(), (install.ok() ? "" : install.error().message));
		d->install = *install;
		auto source = bf3::core::ant::AntInstallSource::open(d->install);
		REQUIRE_MESSAGE(source.ok(), (source.ok() ? "" : source.error().message));
		d->source = std::move(*source);
		auto t0 = std::chrono::steady_clock::now();
		auto lib = load_ant_library(*d->source);
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? "" : lib.error().message));
		d->library = std::move(*lib);
		auto t1 = std::chrono::steady_clock::now();
		auto clips = load_crowd_clips(*d->source, *d->library);
		REQUIRE_MESSAGE(clips.ok(), (clips.ok() ? "" : clips.error().message));
		d->clips = std::move(*clips);
		auto moves = load_crowd_move_clips(d->clips, *d->library);
		REQUIRE_MESSAGE(moves.ok(), (moves.ok() ? "" : moves.error().message));
		d->moves = std::move(*moves);
		auto t2 = std::chrono::steady_clock::now();
		auto groups = load_crowd_move_groups(d->clips, *d->library);
		REQUIRE_MESSAGE(groups.ok(), (groups.ok() ? "" : groups.error().message));
		d->groups = std::move(*groups);
		auto t3 = std::chrono::steady_clock::now();
		d->seconds_library = std::chrono::duration<double>(t1 - t0).count();
		d->seconds_clips = std::chrono::duration<double>(t2 - t1).count();
		d->seconds_groups = std::chrono::duration<double>(t3 - t2).count();
		d->ok = true;
		return d;
	}();
	return data;
}

Pt1Data *pt1_or_skip() {
	Pt1Data *d = pt1_data();
	if (!d->ok) {
		bf3::test::skip_install_case(d->why.c_str());
		return nullptr;
	}
	return d;
}

std::string moving_controller(const MoveGroupDef &p_def, float p_x) {
	for (const MoveGroupDirection &d : p_def.directions) {
		if (std::fabs(d.x - p_x) < 1e-3f) {
			for (const MoveGroupEntry &e : d.entries) {
				if (e.velocity > 0.0f) {
					return p_def.clips[e.clip].controller;
				}
			}
		}
	}
	return "";
}

const MoveGroupEntry *moving_entry(const MoveGroupDef &p_def, float p_x) {
	for (const MoveGroupDirection &d : p_def.directions) {
		if (std::fabs(d.x - p_x) < 1e-3f) {
			for (const MoveGroupEntry &e : d.entries) {
				if (e.velocity > 0.0f) {
					return &e;
				}
			}
		}
	}
	return nullptr;
}

struct Gait {
	const char *what;
	uint8_t stance;
	Vec3 velocity;
	bool sprinting;
	CrowdMoveGroupRole group;
	float x;
};

using FeetTrack = std::vector<std::array<Vec3, 2>>;

struct GaitResult {
	double cycle_seconds = 0;
	double stride_ratio = 0;
	double slide_mps = 0;
	uint32_t plants = 0;
};

constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr int kGaitFrames = 300, kGaitSkip = 60;

GaitResult feet_metrics(const FeetTrack &p_local, const Vec3 &p_velocity, float p_band) {
	const double dt = kFrameSeconds;
	const float speed = std::sqrt(p_velocity.x * p_velocity.x + p_velocity.z * p_velocity.z);
	GaitResult r;
	double slide = 0, model_speed = 0;
	uint32_t n = 0;
	std::vector<int> plant_starts;
	const int frames = int(p_local.size());
	for (int k = 0; k < 2; ++k) {
		float lo = 1e9f;
		for (int f = kGaitSkip; f < frames; ++f) {
			lo = std::min(lo, p_local[size_t(f)][size_t(k)].y);
		}
		bool was = false;
		for (int f = kGaitSkip; f < frames; ++f) {
			const Vec3 &lb = p_local[size_t(f)][size_t(k)];
			const bool planted = lb.y < lo + p_band;
			if (planted && !was && k == 0 && f > kGaitSkip) {
				plant_starts.push_back(f);
			}
			if (planted && was && speed > 0.0f) {
				const Vec3 &la = p_local[size_t(f - 1)][size_t(k)];
				const double wx = (lb.x - la.x) + p_velocity.x * dt, wz = (lb.z - la.z) + p_velocity.z * dt;
				slide += std::sqrt(wx * wx + wz * wz) / dt;
				model_speed += -((lb.x - la.x) * p_velocity.x + (lb.z - la.z) * p_velocity.z) / (speed * dt);
				++n;
			}
			was = planted;
		}
	}
	if (n > 0) {
		r.slide_mps = slide / n;
		r.stride_ratio = model_speed / n / speed;
	}
	r.plants = uint32_t(plant_starts.size());
	if (plant_starts.size() >= 2) {
		r.cycle_seconds = double(plant_starts.back() - plant_starts.front()) * dt / double(plant_starts.size() - 1);
	}
	return r;
}

SkinBinds rig_binds(const Skeleton &p_rig) {
	SkinBinds binds;
	EbxSkeleton ebx;
	ebx.partition = "rig";
	std::vector<Xform> model;
	model_pose(p_rig, rest_pose(p_rig), model);
	for (size_t i = 0; i < p_rig.size(); ++i) {
		ebx.bone_names.push_back(p_rig.bone(i).name);
		ebx.parents.push_back(p_rig.bone(i).parent);
		ebx.local_pose.push_back(mat_from_xform(p_rig.bone(i).rest));
		ebx.model_pose.push_back(mat_from_xform(model[i]));
	}
	for (uint32_t i = 0; i < 4; ++i) {
		REQUIRE(binds.bind(ebx, i, p_rig).ok());
	}
	return binds;
}

FeetTrack evaluated_feet(const Pt1Data &p_d, const CrowdMoveGroupSet *p_groups, const Gait &p_gait, const int32_t p_feet[2]) {
	const Skeleton &rig = p_d.clips.rig;
	const SkinBinds binds = rig_binds(rig);
	CrowdTeam t;
	t.binds = &binds;
	const std::vector<CrowdTeam> teams{ t, t };
	CrowdStanceSet stance;
	stance.moves = &p_d.moves;
	stance.move_groups = p_groups;
	CrowdConfig cfg;
	cfg.temporal_lod = false;
	auto e = CrowdEvaluator::make(p_d.clips, stance, teams, 1, cfg);
	REQUIRE(e.ok());
	FeetTrack out(kGaitFrames);
	if (!e.ok()) {
		return out;
	}
	CrowdSoldierInput in;
	in.velocity = p_gait.velocity;
	in.sprinting = p_gait.sprinting;
	in.pose = in.target_pose = p_gait.stance;
	for (int f = 0; f < kGaitFrames; ++f) {
		in.position = Vec3{ p_gait.velocity.x * float(f * kFrameSeconds), 0.0f, p_gait.velocity.z * float(f * kFrameSeconds) };
		(*e)->set_inputs(std::span<const CrowdSoldierInput>(&in, 1), kFrameSeconds);
		(*e)->evaluate_all_here();
		const std::span<const Xform> m = (*e)->model(0);
		REQUIRE(m.size() == rig.size());
		for (int k = 0; k < 2 && m.size() == rig.size(); ++k) {
			out[size_t(f)][size_t(k)] = m[size_t(p_feet[k])].t;
		}
	}
	return out;
}

FeetTrack authored_feet(const Pt1Data &p_d, const CrowdMoveGroup &p_group, uint32_t p_clip, const int32_t p_feet[2], Vec3 &r_velocity) {
	const Skeleton &rig = p_d.clips.rig;
	const CrowdClipData &c = p_group.clips[p_clip];
	r_velocity = Vec3{ p_group.root_dir[p_clip].x * p_group.timing[p_clip].root_velocity * kAntTicksPerSecond, 0.0f,
		p_group.root_dir[p_clip].z * p_group.timing[p_clip].root_velocity * kAntTicksPerSecond };
	FeetTrack out(kGaitFrames);
	LocalPose pose;
	bf3::core::ant::ClipPose scratch;
	std::vector<Xform> model;
	const float end = float(c.tracks.end_frame);
	for (int f = 0; f < kGaitFrames; ++f) {
		float frame = float(f * kFrameSeconds) * c.tracks.fps;
		frame -= end * std::floor(frame / end);
		sample_pose(rig, c.clip, c.tracks, frame, pose, scratch);
		pose.joints[size_t(p_d.clips.root_joint)].t = rig.bone(size_t(p_d.clips.root_joint)).rest.t;
		model_pose(rig, pose, model);
		for (int k = 0; k < 2; ++k) {
			out[size_t(f)][size_t(k)] = model[size_t(p_feet[k])].t;
		}
	}
	return out;
}

uint32_t moving_clip(const MoveGroupDef &p_def, float p_x) {
	const MoveGroupEntry *e = moving_entry(p_def, p_x);
	return e != nullptr ? e->clip : 0;
}

}

TEST_SUITE("pt1-run") {
	TEST_CASE("pt1 move groups: directions, chosen controllers, Velocity, ItemSpeed, measured directions") {
		Pt1Data *d = pt1_or_skip();
		if (d == nullptr) {
			return;
		}
		MESSAGE("Ant library " << d->seconds_library << " s, crowd clips and moves " << d->seconds_clips << " s, move groups " << d->seconds_groups << " s");
		for (const std::string &n : d->groups.notes) {
			MESSAGE(n);
		}
		const CrowdMoveGroup &run = d->groups.get(CrowdMoveGroupRole::Run);
		const CrowdMoveGroup &sprint = d->groups.get(CrowdMoveGroupRole::Sprint);
		const CrowdMoveGroup &sneak = d->groups.get(CrowdMoveGroupRole::Sneak);
		REQUIRE(run.ok);
		REQUIRE(sprint.ok);
		REQUIRE(sneak.ok);
		if (!run.ok || !sprint.ok || !sneak.ok) {
			return;
		}

		CHECK(run.def.directions.size() == 6);
		CHECK(run.def.one_to_one_speed == doctest::Approx(1.0));
		CHECK(run.def.min_speed == doctest::Approx(0.01));
		CHECK(moving_controller(run.def, 0.0f) == "Rifle_RunFwd");
		CHECK(moving_controller(run.def, -0.5f * kPi) == "Rifle_RunLeft1");
		CHECK(moving_controller(run.def, 0.5f * kPi) == "Rifle_RunRight2");
		CHECK(moving_controller(run.def, kPi) == "Rifle_RunBwd");
		CHECK(moving_controller(run.def, -kPi) == "Rifle_RunBwd");
		REQUIRE(moving_entry(run.def, 0.0f) != nullptr);
		REQUIRE(moving_entry(run.def, 0.5f * kPi) != nullptr);
		REQUIRE(moving_entry(sprint.def, 0.0f) != nullptr);
		REQUIRE(moving_entry(sneak.def, 0.0f) != nullptr);
		if (moving_entry(run.def, 0.0f) == nullptr || moving_entry(run.def, 0.5f * kPi) == nullptr || moving_entry(sprint.def, 0.0f) == nullptr ||
				moving_entry(sneak.def, 0.0f) == nullptr) {
			return;
		}
		CHECK(moving_entry(run.def, 0.0f)->velocity == doctest::Approx(0.0705144));
		CHECK(moving_entry(run.def, 0.5f * kPi)->item_speed == doctest::Approx(0.911911));
		CHECK(run.def.clips[run.def.directions[0].entries[0].clip].controller == "Rifle_RunFwdToIdlePose");

		CHECK(moving_controller(sprint.def, 0.0f) == "Rifle_Sprint");
		CHECK(moving_controller(sprint.def, -0.25f * kPi) == "Rifle_SprintLeft45");
		CHECK(moving_controller(sprint.def, 0.25f * kPi) == "Rifle_SprintRight45");
		CHECK(moving_entry(sprint.def, 0.0f)->velocity == doctest::Approx(0.0960196));
		CHECK(moving_entry(sprint.def, 0.0f)->item_speed == doctest::Approx(1.11005));
		CHECK(sprint.def.min_speed == 0.0f);

		CHECK(moving_controller(sneak.def, 0.0f) == "Rifle_SneakFwd");
		CHECK(moving_entry(sneak.def, 0.0f)->item_speed == doctest::Approx(1.90921));
		CHECK(moving_controller(sneak.def, kPi) == "Rifle_SneakBwd");
		CHECK(moving_controller(sneak.def, -0.5f * kPi) == "Rifle_SneakLeft2");
		CHECK(moving_controller(sneak.def, 0.5f * kPi) == "Rifle_SneakRight2");

		for (const CrowdMoveGroup *g : { &run, &sprint, &sneak }) {
			CHECK(g->x_sign == -1.0f);
			CHECK(g->worst_direction_deg < 15.0f);
			for (const AntClipRoute &c : g->def.clips) {
				CHECK(c.fps / c.fps_scale == doctest::Approx(kAntTicksPerSecond));
			}
		}

		for (const CrowdMoveGroup *g : { &run, &sprint, &sneak }) {
			for (const MoveGroupDirection &dir : g->def.directions) {
				for (const MoveGroupEntry &e : dir.entries) {
					if (e.velocity <= 0.0f) {
						continue;
					}
					const double root = g->timing[e.clip].root_velocity;
					char line[200];
					std::snprintf(line, sizeof(line), "%-22s X %+.3f: Velocity %.4f m/tick (%.2f m/s), root x ItemSpeed %.4f (%.2f m/s), ratio %.3f",
							g->def.clips[e.clip].controller.c_str(), double(dir.x), double(e.velocity), double(e.velocity) * 60.0, root * e.item_speed,
							root * e.item_speed * 60.0, double(e.velocity) / (root * e.item_speed));
					MESSAGE(line);
				}
			}
		}
	}

	TEST_CASE("pt1 cycle rate and stride per speed, before (S3's run blend) and after (BF3's move groups)") {
		Pt1Data *d = pt1_or_skip();
		if (d == nullptr) {
			return;
		}

		const Gait gaits[] = {
			{ "run fwd 4.0", 0, Vec3{ 0, 0, 4.0f }, false, CrowdMoveGroupRole::Run, 0.0f },
			{ "run left 3.0", 0, Vec3{ 3.0f, 0, 0 }, false, CrowdMoveGroupRole::Run, -0.5f * kPi },
			{ "run right 3.0", 0, Vec3{ -3.0f, 0, 0 }, false, CrowdMoveGroupRole::Run, 0.5f * kPi },
			{ "run bwd 3.0", 0, Vec3{ 0, 0, -3.0f }, false, CrowdMoveGroupRole::Run, kPi },
			{ "sprint fwd 6.5", 0, Vec3{ 0, 0, 6.5f }, true, CrowdMoveGroupRole::Sprint, 0.0f },
			{ "crouch fwd 2.0", 1, Vec3{ 0, 0, 2.0f }, false, CrowdMoveGroupRole::Sneak, 0.0f },
			{ "crouch left 1.4", 1, Vec3{ 1.4f, 0, 0 }, false, CrowdMoveGroupRole::Sneak, -0.5f * kPi },
		};

		const Skeleton &rig = d->clips.rig;
		const int32_t feet[2] = { rig.find("LeftToeBase"), rig.find("RightToeBase") };
		REQUIRE(feet[0] >= 0);
		REQUIRE(feet[1] >= 0);
		if (feet[0] < 0 || feet[1] < 0) {
			return;
		}
		constexpr float kBand = 0.01f;
		for (const Gait &g : gaits) {
			const CrowdMoveGroup &grp = d->groups.get(g.group);
			const uint32_t clip = moving_clip(grp.def, g.x);
			Vec3 v_auth;
			const GaitResult ra = feet_metrics(authored_feet(*d, grp, clip, feet, v_auth), v_auth, kBand);
			const GaitResult rb = feet_metrics(evaluated_feet(*d, nullptr, g, feet), g.velocity, kBand);
			const GaitResult rf = feet_metrics(evaluated_feet(*d, &d->groups, g, feet), g.velocity, kBand);
			const float speed = std::sqrt(g.velocity.x * g.velocity.x + g.velocity.z * g.velocity.z);
			const MoveGroupWeights w = move_group_weights(grp.def, grp.timing, move_group_x(g.velocity, grp.x_sign), speed, d->groups.options);
			char line[400];
			std::snprintf(line, sizeof(line),
					"%-16s %-20s authored stride/ground %.3f | before: cycle %.3f s (%3.0f steps/min), stride/ground %.3f, toe slide %.2f m/s | after: cycle "
					"%.3f s (%3.0f steps/min; data %.3f s), stride/ground %.3f, toe slide %.2f m/s",
					g.what, grp.def.clips[clip].controller.c_str(), ra.stride_ratio, rb.cycle_seconds, rb.cycle_seconds > 0 ? 120.0 / rb.cycle_seconds : 0.0,
					rb.stride_ratio, rb.slide_mps, rf.cycle_seconds, rf.cycle_seconds > 0 ? 120.0 / rf.cycle_seconds : 0.0,
					w.cycles_per_second > 0 ? 1.0 / w.cycles_per_second : 0.0, rf.stride_ratio, rf.slide_mps);
			MESSAGE(line);
			CHECK(rf.plants >= 3);

			CHECK(rf.stride_ratio == doctest::Approx(ra.stride_ratio).epsilon(0.03));
			REQUIRE(w.cycles_per_second > 0.0f);
			CHECK(rf.cycle_seconds == doctest::Approx(1.0 / w.cycles_per_second).epsilon(0.04));
		}

		const CrowdMoveGroup &run = d->groups.get(CrowdMoveGroupRole::Run);
		const MoveGroupWeights w4 = move_group_weights(run.def, run.timing, 0.0f, 4.0f, d->groups.options);
		CHECK(1.0 / w4.cycles_per_second == doctest::Approx(0.6).epsilon(0.01));
		const CrowdMoveGroup &sprint = d->groups.get(CrowdMoveGroupRole::Sprint);
		const MoveGroupWeights w65 = move_group_weights(sprint.def, sprint.timing, 0.0f, 6.5f, d->groups.options);
		MESSAGE("sprint at 6.5 m/s: rate " << w65.terms[0].rate << ", cycle " << 1.0 / w65.cycles_per_second << " s");
		const LocoWeights s3 = locomotion_weights(d->clips.loco, Vec3{ 0, 0, 6.5f }, LocoParams{});
		MESSAGE("S3's run blend at 6.5 m/s: Rifle_RunFwd at " << 0.6 / s3.cycle_seconds << "x, cycle " << s3.cycle_seconds << " s");
		CHECK(w65.count == 1);
		CHECK(sprint.def.clips[w65.terms[0].clip].controller == "Rifle_Sprint");
	}

	TEST_CASE("1p deltas: routes per weapon type, cadence and amplitude before and after") {
		Pt1Data *d = pt1_or_skip();
		if (d == nullptr) {
			return;
		}
		const bf3::core::ant::AntLibrary &lib = *d->library;
		const bf3::core::ant::AntRig *ant_rig = nullptr;
		for (const bf3::core::ant::AntRig &r : lib.rigs()) {
			if (r.name == "Venice1pAntSke01") {
				ant_rig = &r;
			}
		}
		REQUIRE(ant_rig != nullptr);
		if (ant_rig == nullptr) {
			return;
		}
		bf3::core::Result<Skeleton> rig = skeleton_from_rig(*ant_rig);
		REQUIRE(rig.ok());
		if (!rig.ok()) {
			return;
		}

		auto ant_ref = [&](std::string_view p_name) {
			const bf3::core::ant::AntPackage &sp = *lib.static_package();
			for (size_t i = 0; i < sp.object_count(); ++i) {
				const bf3::core::ant::GdStruct o = sp.object(i);
				if (o.type_name() == "WeaponAsset" && o.name() == p_name) {
					return 0x15480000u | (o.guid().index() & 0xFFFFu);
				}
			}
			return 0u;
		};
		struct Want {
			const char *weapon;
			uint32_t type;
		};
		const Want wants[] = { { "M16A2", 0 }, { "Fnminimi", 3 }, { "Smaw", 4 }, { "Glock18", 5 }, { "M40a5", 6 }, { "M1014", 7 } };
		std::vector<std::shared_ptr<const bf3::render::ViewmodelLocomotionClips>> loaded;
		for (const Want &w : wants) {
			bf3::render::ViewmodelItem item;
			item.unlock = w.weapon;
			std::vector<std::string> notes;
			const uint32_t ref = ant_ref(w.weapon);
			REQUIRE(ref != 0u);
			bf3::render::load_viewmodel_locomotion(lib, *rig, ref, item, notes);
			for (const std::string &n : notes) {
				MESSAGE(n);
			}
			REQUIRE(item.locomotion != nullptr);
			if (!item.locomotion) {
				continue;
			}
			CHECK(item.locomotion->weapon_type == w.type);
			MESSAGE((std::string(w.weapon) + " run: " + item.locomotion->run_path));
			MESSAGE((std::string(w.weapon) + " sprint: " + item.locomotion->sprint_path));
			loaded.push_back(item.locomotion);
		}
		REQUIRE(!loaded.empty());
		if (loaded.empty()) {
			return;
		}
		const bf3::render::ViewmodelLocomotionClips &m16 = *loaded[0];
		CHECK(m16.run.controller == "Rifle_RunFwdDelta");
		CHECK(m16.run_start_frame == doctest::Approx(14.0f));
		CHECK(m16.run_end_frame == doctest::Approx(90.0f));
		CHECK(m16.sprint.controller == "Rifle_SprintDelta");
		CHECK(m16.sprint_start_frame == doctest::Approx(0.0f));
		CHECK(m16.sprint_end_frame == doctest::Approx(72.0f));

		const int32_t cam = rig->find("CameraJoint"), wep = rig->find("Wep_Root");
		REQUIRE(cam >= 0);
		REQUIRE(wep >= 0);
		if (cam < 0 || wep < 0) {
			return;
		}
		struct Axis {
			double hz = 0, p2p_mm = 0;
		};
		auto measure = [](const std::vector<Vec3> &p_track) {
			std::array<Axis, 3> out{};
			const double seconds = double(p_track.size()) / 60.0;
			for (int k = 0; k < 3; ++k) {
				auto at = [&](size_t i) { return double(k == 0 ? p_track[i].x : k == 1 ? p_track[i].y : p_track[i].z); };
				double mean = 0, lo = 1e9, hi = -1e9;
				for (size_t i = 0; i < p_track.size(); ++i) {
					mean += at(i);
					lo = std::min(lo, at(i));
					hi = std::max(hi, at(i));
				}
				mean /= double(p_track.size());
				uint32_t ups = 0;
				for (size_t i = 1; i < p_track.size(); ++i) {
					ups += (at(i - 1) < mean && at(i) >= mean) ? 1 : 0;
				}
				out[size_t(k)].hz = double(ups) / seconds;
				out[size_t(k)].p2p_mm = (hi - lo) * 1000.0;
			}
			return out;
		};
		const ViewmodelTuning tuning;
		const std::array<VmClipRef, kVmClips> no_clips{};
		struct Case {
			const char *what;
			float speed;
			bool sprinting;
		};
		const Case cases[] = { { "run 4.0", 4.0f, false }, { "sprint 6.5", 6.5f, true } };
		for (const Case &c : cases) {
			std::vector<Vec3> before, after;
			for (int pass = 0; pass < 2; ++pass) {
				ViewmodelAnimator a(*rig, no_clips, tuning);
				if (pass == 1) {
					a.set_locomotion(bf3::render::viewmodel_locomotion([&] {
						bf3::render::ViewmodelItem it;
						it.locomotion = loaded[0];
						return it;
					}()));
				}
				ViewmodelFrameInput in;
				in.speed = c.speed;
				in.sprinting = c.sprinting;
				LocalPose pose;
				ViewmodelOutput out;
				std::vector<Xform> model;
				for (int f = 0; f < 660; ++f) {
					in.time_s += 1.0 / 60.0;
					in.dt = 1.0f / 60.0f;
					a.step(in, {}, pose, out);
					if (f < 60) {
						continue;
					}
					if (pass == 0) {
						before.push_back(out.weapon_offset.t);
					} else {
						model_pose(*rig, pose, model);
						after.push_back(xform_mul(xform_inverse(model[size_t(cam)]), model[size_t(wep)]).t);
						CHECK(out.loco_run_weight + out.loco_sprint_weight == doctest::Approx(1.0f));
					}
				}
			}
			const std::array<Axis, 3> b = measure(before), af = measure(after);
			char line[300];
			std::snprintf(line, sizeof(line),
					"%-10s before (bob): x %.2f Hz %.1f mm, y %.2f Hz %.1f mm | after (deltas): x %.2f Hz %.1f mm, y %.2f Hz %.1f mm, z %.2f Hz %.1f mm", c.what,
					b[0].hz, b[0].p2p_mm, b[1].hz, b[1].p2p_mm, af[0].hz, af[0].p2p_mm, af[1].hz, af[1].p2p_mm, af[2].hz, af[2].p2p_mm);
			MESSAGE(line);
			if (c.sprinting) {

				CHECK(af[0].hz == doctest::Approx(30.0 / 18.0).epsilon(0.08));
				CHECK(af[1].hz == doctest::Approx(30.0 / 9.0).epsilon(0.08));
				CHECK(b[0].hz > 1.5 * af[0].hz);
			} else {

				CHECK(af[0].hz == doctest::Approx(1.0 / 0.6).epsilon(0.08));
			}
		}
	}

	TEST_CASE("install unchanged") {
		Pt1Data *d = pt1_or_skip();
		if (d == nullptr) {
			return;
		}
		const bf3::core::SnapshotDiff diff = bf3::core::diff_snapshots(d->before, d->reader.snapshot());
		CHECK(diff.clean());
	}
}

namespace {

Skeleton vm_rig() {
	std::vector<Bone> bones(3);
	bones[0].name = "Root";
	bones[1].name = "CameraJoint";
	bones[1].parent = 0;
	bones[1].rest = Xform{ Quat{}, Vec3{ 0, 0.1f, 0 } };
	bones[2].name = "Wep_Root";
	bones[2].parent = 0;
	bones[2].rest = Xform{ Quat{}, Vec3{ 0, 0, 0.3f } };
	return *Skeleton::make(std::move(bones));
}

void vm_ramp(bf3::core::ant::DecodedClip &r_clip, ClipTracks &r_tracks, uint16_t p_frames, float p_deg) {
	r_clip = {};
	r_clip.codec = bf3::core::ant::ClipCodec::Raw;
	r_clip.additive = true;
	r_clip.num_keys = 2;
	r_clip.num_quats = 1;
	r_clip.end_frame = p_frames;
	r_clip.key_times = { 0, p_frames };
	r_clip.quats = { Quat{}, axis_angle(0, 1, 0, p_deg) };
	r_tracks = {};
	r_tracks.rotation_bone = { 2 };
	r_tracks.fps = 30.0f;
	r_tracks.end_frame = p_frames;
	r_tracks.channels_bound = 1;
}

float y_deg(const Quat &p_q) {
	return 2.0f * std::atan2(p_q.y, p_q.w) * 180.0f / kPi;
}

struct VmSynth {
	Skeleton rig = vm_rig();
	bf3::core::ant::DecodedClip run_clip, sprint_clip;
	ClipTracks run_tracks, sprint_tracks;
	std::array<VmClipRef, kVmClips> refs{};
	VmLocomotion loco;

	VmSynth() {
		vm_ramp(run_clip, run_tracks, 60, 30.0f);
		vm_ramp(sprint_clip, sprint_tracks, 40, -40.0f);
		loco.run.clip.clip = &run_clip;
		loco.run.clip.tracks = &run_tracks;
		loco.run.start_frame = 14.0f;
		loco.run.end_frame = 44.0f;
		loco.sprint.clip.clip = &sprint_clip;
		loco.sprint.clip.tracks = &sprint_tracks;
		loco.sprint.start_frame = 0.0f;
		loco.sprint.end_frame = 40.0f;
	}
};

struct VmRun {
	LocalPose pose;
	ViewmodelOutput out;
};

VmRun vm_steps(ViewmodelAnimator &r_a, ViewmodelFrameInput &r_in, int p_steps) {
	VmRun r;
	for (int i = 0; i < p_steps; ++i) {
		r_in.time_s += 1.0 / 60.0;
		r_in.dt = 1.0f / 60.0f;
		r_a.step(r_in, {}, r.pose, r.out);
	}
	return r;
}

}

TEST_SUITE("pt1-run-unit") {
	TEST_CASE("1p deltas: run rate per speed, sprint at rate 1, weights, the bob replaced") {
		VmSynth s;
		const ViewmodelTuning tuning;
		ViewmodelAnimator a(s.rig, s.refs, tuning);
		a.set_locomotion(s.loco);
		ViewmodelFrameInput in;
		in.speed = kVmRunSpeedUnit;

		VmRun r = vm_steps(a, in, 30);
		CHECK(r.out.loco_run_frame == doctest::Approx(29.0f).epsilon(0.002));
		CHECK(r.out.loco_run_weight == doctest::Approx(1.0f));
		CHECK(r.out.loco_sprint_weight == 0.0f);
		CHECK(y_deg(r.pose.joints[2].q) == doctest::Approx(14.5f).epsilon(0.01));
		CHECK(std::fabs(r.out.weapon_offset.t.x) < 1e-6f);
		CHECK(std::fabs(r.out.weapon_offset.t.y) < 1e-6f);

		in.speed = 2.0f * kVmRunSpeedUnit;
		r = vm_steps(a, in, 30);
		CHECK(r.out.loco_run_frame == doctest::Approx(29.0f).epsilon(0.002));

		in.speed = 4.0f;
		const float f0 = r.out.loco_run_frame;
		r = vm_steps(a, in, 6);
		CHECK(r.out.loco_run_frame - f0 == doctest::Approx(0.1f * 30.0f * 4.0f / kVmRunSpeedUnit).epsilon(0.002));

		in.ads = 1.0f;
		r = vm_steps(a, in, 1);
		CHECK(r.out.loco_run_weight == 0.0f);
		in.ads = 0.0f;

		in.sprinting = true;
		in.speed = 6.5f;
		r = vm_steps(a, in, 6);
		CHECK(r.out.loco_sprint_weight == doctest::Approx(0.5f).epsilon(0.01));
		CHECK(r.out.loco_run_weight == doctest::Approx(0.5f).epsilon(0.01));
		const float s0 = r.out.loco_sprint_frame;
		r = vm_steps(a, in, 18);
		CHECK(r.out.loco_sprint_weight == doctest::Approx(1.0f));
		CHECK(r.out.loco_run_weight == 0.0f);
		CHECK(r.out.loco_sprint_frame - s0 == doctest::Approx(9.0f).epsilon(0.002));
		CHECK(y_deg(r.pose.joints[2].q) == doctest::Approx(-r.out.loco_sprint_frame).epsilon(0.01));

		in.sprinting = false;
		in.speed = 0.05f;
		r = vm_steps(a, in, 13);
		CHECK(r.out.loco_run_weight == 0.0f);
		CHECK(r.out.loco_sprint_weight == 0.0f);
		CHECK(y_deg(r.pose.joints[2].q) == doctest::Approx(0.0f));
		in.speed = 4.0f;
		in.grounded = false;
		r = vm_steps(a, in, 12);
		CHECK(r.out.loco_run_weight == 0.0f);

		a.reset();
		in.grounded = true;
		in.time_s = 0;
		r = vm_steps(a, in, 1);
		CHECK(r.out.loco_run_frame == doctest::Approx(14.0f + 30.0f * 4.0f / kVmRunSpeedUnit / 60.0f).epsilon(0.002));
	}

	TEST_CASE("1p deltas: without them the procedural bob; hostile input") {
		VmSynth s;
		const ViewmodelTuning tuning;
		{
			ViewmodelAnimator a(s.rig, s.refs, tuning);
			ViewmodelFrameInput in;
			in.speed = 4.0f;
			const VmRun r = vm_steps(a, in, 40);
			CHECK(r.out.loco_run_weight == 0.0f);
			CHECK(std::fabs(r.out.weapon_offset.t.x) + std::fabs(r.out.weapon_offset.t.y) > 1e-4f);
		}
		{

			ViewmodelAnimator a(s.rig, s.refs, tuning);
			a.set_locomotion(s.loco);
			a.set_locomotion(VmLocomotion{});
			VmLocomotion bad = s.loco;
			ClipTracks empty_tracks = s.run_tracks;
			empty_tracks.end_frame = 0;
			bad.run.clip.tracks = &empty_tracks;
			bad.sprint.clip.clip = nullptr;
			a.set_locomotion(bad);
			ViewmodelFrameInput in;
			in.speed = 4.0f;
			const VmRun r = vm_steps(a, in, 40);
			CHECK(r.out.loco_run_weight == 0.0f);
			CHECK(std::fabs(r.out.weapon_offset.t.x) + std::fabs(r.out.weapon_offset.t.y) > 1e-4f);
		}
		{

			ViewmodelAnimator a(s.rig, s.refs, tuning);
			VmLocomotion odd = s.loco;
			odd.run.start_frame = std::numeric_limits<float>::quiet_NaN();
			odd.sprint.start_frame = 500.0f;
			odd.sprint.end_frame = -3.0f;
			a.set_locomotion(odd);
			ViewmodelFrameInput in;
			in.speed = std::numeric_limits<float>::quiet_NaN();
			VmRun r = vm_steps(a, in, 5);
			CHECK(std::isfinite(r.out.loco_run_frame));
			in.speed = 1.0e30f;
			r = vm_steps(a, in, 5);
			CHECK(std::isfinite(r.out.loco_run_frame));
			CHECK(r.out.loco_run_frame >= 0.0f);
			CHECK(r.out.loco_run_frame < 60.0f);
			in.sprinting = true;
			in.speed = 6.5f;
			r = vm_steps(a, in, 30);
			CHECK(r.out.loco_sprint_frame >= 0.0f);
			CHECK(r.out.loco_sprint_frame < 40.0f);
			for (const Xform &x : r.pose.joints) {
				CHECK(std::isfinite(x.q.w));
				CHECK(std::isfinite(x.t.z));
			}
		}
	}
}
