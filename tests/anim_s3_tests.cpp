// S3 crowd animation (docs/design/s3.md §14): bf3_anim's crowd pieces and bf3_core's WorkerGroup.
//
// Suite "anim-s3-unit" (ctest anim.s3.unit): synthetic skeletons and clips, no data. Layers, the run blend, the worker
// lanes, the evaluator's frame protocol on a 4-bone rig, and the stage schedule and its blend coverage.
// Suite "anim-s3" (ctest anim.s3): the retail install, read-only (Ant banks through AntInstallSource); exits 77
// without one. The clip set of §4.1, weapon -> chooser index, the reload clips and mask, the additive-order criterion
// of §5.5, the 64-soldier bench (reported) and seeded hostile lookups. Asserts numbers only; nothing from BF3 is
// stored in the repo.
#include <doctest/doctest.h>

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/crowd_stage.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/locomotion.h"
#include "bf3/anim/temporal_lod.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/worker_group.h"
#include "test_support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace bf3::anim;
using bf3::core::WorkerGroup;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

Quat axis_angle(float x, float y, float z, float p_deg) {
	const float h = p_deg * kPi / 360.0f;
	const float s = std::sin(h);
	return Quat{ x * s, y * s, z * s, std::cos(h) };
}

bool same_quat(const Quat &a, const Quat &b, double p_eps = 1e-5) {
	return quat_angle_deg(a, b) < p_eps * 57.3 + 1e-4;
}

bool bit_equal(const Xform &a, const Xform &b) {
	return std::memcmp(&a, &b, sizeof(Xform)) == 0;
}

// AITrajectory (root) > Hips > Arm > Wep; Hips 1 m up.
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

// A two-key clip: quaternion channels on p_qbones (key 0 identity, key 1 p_q1), vec3 channels on p_vbones (key 0
// p_t0, key 1 p_t1).
CrowdClipData synth_clip(const Skeleton &p_rig, uint32_t p_end_frame, std::vector<int32_t> p_qbones, const Quat &p_q1, std::vector<int32_t> p_vbones,
		const Vec3 &p_t0, const Vec3 &p_t1, bool p_additive = false) {
	CrowdClipData d;
	bf3::core::ant::DecodedClip &c = d.clip;
	c.codec = bf3::core::ant::ClipCodec::Raw;
	c.num_keys = 2;
	c.num_quats = uint32_t(p_qbones.size());
	c.num_vec3s = uint32_t(p_vbones.size());
	c.end_frame = p_end_frame;
	c.additive = p_additive;
	c.key_times = { 0, uint16_t(p_end_frame) };
	for (uint32_t k = 0; k < 2; ++k) {
		for (size_t q = 0; q < p_qbones.size(); ++q) {
			c.quats.push_back(k == 0 ? Quat{} : p_q1);
		}
		for (size_t v = 0; v < p_vbones.size(); ++v) {
			c.vec3s.push_back(k == 0 ? p_t0 : p_t1);
		}
	}
	d.tracks.rotation_bone = std::move(p_qbones);
	d.tracks.translation_bone = std::move(p_vbones);
	d.tracks.fps = 30.0f;
	d.tracks.end_frame = p_end_frame;
	d.mask = mask_from_tracks(p_rig, d.tracks);
	d.additive = p_additive;
	return d;
}

struct SynthCrowd {
	CrowdClipSet set;
	SkinBinds binds;
	CrowdClipData reload;
	BoneMask weapon_joints;
	std::vector<Xform> weapon_local;
	std::vector<CrowdTeam> teams;

	SynthCrowd() {
		set.rig = synth_rig();
		set.root_joint = 0;
		const Skeleton &r = set.rig;
		set.clips[size_t(CrowdClip::Idle)] = synth_clip(r, 1, { 1, 2 }, axis_angle(0, 1, 0, 5), { 1 }, Vec3{ 0, 1, 0 }, Vec3{ 0, 1, 0 });
		const struct {
			CrowdClip clip;
			Vec3 to;
			float deg;
		} cycles[] = { { CrowdClip::RunFwd, { 0, 0, 2.8f }, 30 }, { CrowdClip::RunBwd, { 0, 0, -1.7f }, -30 }, { CrowdClip::RunLeft, { 1.5f, 0, 0 }, 20 },
			{ CrowdClip::RunRight, { -1.7f, 0, 0 }, -20 } };
		for (const auto &c : cycles) {
			// Root motion on AITrajectory, a swing on Hips and Arm.
			set.clips[size_t(c.clip)] = synth_clip(r, 18, { 1, 2 }, axis_angle(1, 0, 0, c.deg), { 0, 1 }, Vec3{}, c.to);
			// The second vec3 channel drives Hips: keep it at its rest height.
			auto &vs = set.clips[size_t(c.clip)].clip.vec3s;
			vs[1] = Vec3{ 0, 1, 0 };
			vs[3] = Vec3{ 0, 1, 0 };
		}
		set.clips[size_t(CrowdClip::AimUp)] = synth_clip(r, 1, { 2 }, axis_angle(0, 0, 1, 30), {}, Vec3{}, Vec3{}, true);
		set.clips[size_t(CrowdClip::AimDown)] = synth_clip(r, 1, { 2 }, axis_angle(0, 0, 1, -30), {}, Vec3{}, Vec3{}, true);
		set.loco[size_t(LocoClip::Fwd)] = LocoClipInfo{ 0.6f, 4.64f, Vec3{ 0, 0, 1 } };
		set.loco[size_t(LocoClip::Bwd)] = LocoClipInfo{ 0.567f, 2.97f, Vec3{ 0, 0, -1 } };
		set.loco[size_t(LocoClip::Left)] = LocoClipInfo{ 0.567f, 2.6f, Vec3{ 1, 0, 0 } };
		set.loco[size_t(LocoClip::Right)] = LocoClipInfo{ 0.533f, 3.2f, Vec3{ -1, 0, 0 } };
		reload = synth_clip(r, 30, { 2, 3 }, axis_angle(0, 1, 0, 60), { 3 }, Vec3{ 0.3f, 0, 0 }, Vec3{ 0.3f, 0.2f, 0 });
		// Binds: every bone, inverse bind = inverse(rest model).
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
		weapon_joints.weight.assign(r.size(), 0.0f);
		weapon_joints.weight[3] = 1.0f;
		weapon_local.assign(r.size(), xform_identity());
		weapon_local[3] = Xform{ axis_angle(0, 1, 0, 90), Vec3{ 0.25f, 0.05f, 0 } };
		CrowdTeam t;
		t.binds = &binds;
		t.reload = &reload;
		t.weapon_joints = &weapon_joints;
		t.weapon_local = weapon_local;
		teams = { t, t };
	}
};

std::vector<CrowdSoldierInput> stage_inputs(const CrowdStageSpec &p_spec, double p_t, double p_prev, bool p_spread) {
	std::vector<CrowdSoldierInput> in(p_spec.soldiers());
	for (uint32_t i = 0; i < p_spec.soldiers(); ++i) {
		in[i] = stage_input(p_spec, i, p_t, p_prev, stage_camera(p_spec).position, p_spread);
	}
	return in;
}

} // namespace

TEST_SUITE("anim-s3-unit") {
	TEST_CASE("s3 layers: blends, masked override, additive orders") {
		const Skeleton rig = synth_rig();
		LocalPose a = rest_pose(rig), b = rest_pose(rig), out;
		a.joints[2].q = axis_angle(0, 0, 1, 10);
		b.joints[2].q = axis_angle(0, 0, 1, 50);
		b.joints[2].t = Vec3{ 1, 2, 3 };
		blend_poses(a, b, 0.0f, out);
		CHECK(same_quat(out.joints[2].q, a.joints[2].q));
		blend_poses(a, b, 1.0f, out);
		CHECK(same_quat(out.joints[2].q, b.joints[2].q));
		CHECK(vec_dist(out.joints[2].t, b.joints[2].t) < 1e-6);
		blend_poses(a, b, 0.5f, out);
		CHECK(quat_angle_deg(out.joints[2].q, axis_angle(0, 0, 1, 30)) < 0.01);
		// Shortest arc: b negated gives the same blend.
		LocalPose bn = b;
		bn.joints[2].q = Quat{ -b.joints[2].q.x, -b.joints[2].q.y, -b.joints[2].q.z, -b.joints[2].q.w };
		LocalPose out2;
		blend_poses(a, bn, 0.5f, out2);
		CHECK(quat_angle_deg(out2.joints[2].q, out.joints[2].q) < 1e-3);
		// N-way: weights normalised, tiny weights skipped, aliasing the first input.
		const LocalPose *poses[3] = { &a, &b, &bn };
		const float weights[3] = { 2.0f, 2.0f, 0.0f };
		LocalPose n3;
		blend_poses(poses, weights, 3, n3);
		CHECK(quat_angle_deg(n3.joints[2].q, axis_angle(0, 0, 1, 30)) < 0.01);
		LocalPose alias = a;
		const LocalPose *ap[2] = { &alias, &b };
		const float aw[2] = { 1.0f, 1.0f };
		blend_poses(ap, aw, 2, alias);
		CHECK(quat_angle_deg(alias.joints[2].q, axis_angle(0, 0, 1, 30)) < 0.01);

		// Masked override: unmasked bones bit-equal, masked bones take the layer at w = 1.
		BoneMask mask;
		mask.weight = { 0, 0, 1, 0 };
		LocalPose base = a;
		override_masked(base, b, mask, 1.0f);
		CHECK(bit_equal(base.joints[0], a.joints[0]));
		CHECK(bit_equal(base.joints[1], a.joints[1]));
		CHECK(bit_equal(base.joints[3], a.joints[3]));
		CHECK(bit_equal(base.joints[2], b.joints[2]));
		base = a;
		override_masked(base, b, mask, 0.0f);
		CHECK(bit_equal(base.joints[2], a.joints[2]));
		CHECK(mask.count_nonzero() == 1);
		BoneMask other;
		other.weight = { 1, 0, 0 };
		const BoneMask u = mask_union(mask, other);
		CHECK(u.size() == 4);
		CHECK(u.count_nonzero() == 2);

		// Additive: post q * d, pre d * q, against hand-computed products.
		LocalPose delta = identity_pose(rig);
		delta.joints[2] = Xform{ axis_angle(1, 0, 0, 40), Vec3{ 0, 0.1f, 0 } };
		const Quat q = a.joints[2].q;
		LocalPose post = a, pre = a;
		apply_additive(post, delta, mask, 1.0f, AdditiveOrder::LocalPost);
		apply_additive(pre, delta, mask, 1.0f, AdditiveOrder::LocalPre);
		CHECK(same_quat(post.joints[2].q, quat_mul(q, delta.joints[2].q)));
		CHECK(same_quat(pre.joints[2].q, quat_mul(delta.joints[2].q, q)));
		CHECK(vec_dist(post.joints[2].t, Vec3{ a.joints[2].t.x, a.joints[2].t.y + 0.1f, a.joints[2].t.z }) < 1e-6);
		CHECK(quat_angle_deg(post.joints[2].q, pre.joints[2].q) > 1.0); // the orders differ for non-commuting axes
		LocalPose half = a;
		apply_additive(half, delta, mask, 0.5f, AdditiveOrder::LocalPost);
		CHECK(quat_angle_deg(half.joints[2].q, quat_mul(q, axis_angle(1, 0, 0, 20))) < 0.01);
		CHECK(bit_equal(half.joints[1], a.joints[1]));
		// The identity pose is identity everywhere.
		for (const Xform &x : identity_pose(rig).joints) {
			CHECK(bit_equal(x, xform_identity()));
		}
	}

	TEST_CASE("s3 run blend: cardinal, diagonal, idle fade, rate clamp, hostile input") {
		std::array<LocoClipInfo, kLocoClips> clips{};
		clips[size_t(LocoClip::Fwd)] = LocoClipInfo{ 0.6f, 4.64f, Vec3{ 0, 0, 1 } };
		clips[size_t(LocoClip::Bwd)] = LocoClipInfo{ 0.567f, 2.97f, Vec3{ 0, 0, -1 } };
		clips[size_t(LocoClip::Left)] = LocoClipInfo{ 0.567f, 2.6f, Vec3{ 1, 0, 0 } };
		clips[size_t(LocoClip::Right)] = LocoClipInfo{ 0.533f, 3.2f, Vec3{ -1, 0, 0 } };
		const LocoParams p;
		auto nonzero = [](const LocoWeights &w) {
			int n = 0;
			for (float x : w.weight) {
				n += x > 0.0f ? 1 : 0;
			}
			return n;
		};
		auto sum = [](const LocoWeights &w) {
			float s = 0;
			for (float x : w.weight) {
				s += x;
			}
			return s;
		};
		// Forward at the motor's 4 m/s: RunFwd only, 0.86x.
		LocoWeights w = locomotion_weights(clips, Vec3{ 0, 0, 4.0f }, p);
		CHECK(w.weight[size_t(LocoClip::Fwd)] == doctest::Approx(1.0f));
		CHECK(nonzero(w) == 1);
		CHECK(w.cycle_seconds == doctest::Approx(0.6f / (4.0f / 4.64f)).epsilon(1e-4));
		// Back at 3 m/s: 1.01x.
		w = locomotion_weights(clips, Vec3{ 0, 0, -3.0f }, p);
		CHECK(w.weight[size_t(LocoClip::Bwd)] == doctest::Approx(1.0f));
		CHECK(w.cycle_seconds == doctest::Approx(0.567f / (3.0f / 2.97f)).epsilon(1e-4));
		// Diagonal forward-left: half and half.
		w = locomotion_weights(clips, Vec3{ 2.0f, 0, 2.0f }, p);
		CHECK(w.weight[size_t(LocoClip::Fwd)] == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK(w.weight[size_t(LocoClip::Left)] == doctest::Approx(0.5f).epsilon(1e-4));
		CHECK(nonzero(w) == 2);
		// Back-right diagonal.
		w = locomotion_weights(clips, Vec3{ -1.0f, 0, -1.0f }, p);
		CHECK(w.weight[size_t(LocoClip::Bwd)] > 0.4f);
		CHECK(w.weight[size_t(LocoClip::Right)] > 0.4f);
		// The bracket across the wrap (+Z is heading 0): slightly right of forward.
		w = locomotion_weights(clips, Vec3{ -1.0f, 0, 3.0f }, p);
		CHECK(w.weight[size_t(LocoClip::Fwd)] > 0.6f);
		CHECK(w.weight[size_t(LocoClip::Right)] > 0.1f);
		// Idle fade: idle only at <= 0.2 m/s, half at 0.6, none at >= 1.
		w = locomotion_weights(clips, Vec3{ 0, 0, 0.1f }, p);
		CHECK(w.weight[size_t(LocoClip::Idle)] == 1.0f);
		CHECK(nonzero(w) == 1);
		CHECK(w.cycle_seconds == 0.0f);
		w = locomotion_weights(clips, Vec3{ 0, 0, 0.6f }, p);
		CHECK(w.weight[size_t(LocoClip::Idle)] == doctest::Approx(0.5f));
		CHECK(w.weight[size_t(LocoClip::Fwd)] == doctest::Approx(0.5f));
		CHECK(w.cycle_seconds == doctest::Approx(0.6f / p.min_rate)); // clamped to the slowest rate
		w = locomotion_weights(clips, Vec3{ 0, 0, 1.0f }, p);
		CHECK(w.weight[size_t(LocoClip::Idle)] == 0.0f);
		// Rate clamp at the top.
		w = locomotion_weights(clips, Vec3{ 0, 0, 20.0f }, p);
		CHECK(w.cycle_seconds == doctest::Approx(0.6f / p.max_rate));
		// Weights sum to 1 everywhere on a circle of speeds.
		for (int k = 0; k < 360; k += 7) {
			for (float s : { 0.0f, 0.3f, 0.9f, 3.5f }) {
				const LocoWeights x = locomotion_weights(clips, Vec3{ s * std::sin(k * kPi / 180.0f), 0, s * std::cos(k * kPi / 180.0f) }, p);
				CHECK(sum(x) == doctest::Approx(1.0f).epsilon(1e-4));
				CHECK(nonzero(x) <= 3);
			}
		}
		// Hostile numbers: idle.
		const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
		for (const Vec3 v : { Vec3{ nan, 0, 0 }, Vec3{ 0, 0, inf }, Vec3{ -inf, nan, inf } }) {
			w = locomotion_weights(clips, v, p);
			CHECK(w.weight[size_t(LocoClip::Idle)] == 1.0f);
			CHECK(w.cycle_seconds == 0.0f);
		}
		// No cycles at all: idle.
		w = locomotion_weights(std::array<LocoClipInfo, kLocoClips>{}, Vec3{ 0, 0, 3 }, p);
		CHECK(w.weight[size_t(LocoClip::Idle)] == 1.0f);
	}

	TEST_CASE("s3 worker group: every item once, many kicks, busy refusal, destructor waits") {
		for (const size_t n : { size_t(0), size_t(1), size_t(64), size_t(1000) }) {
			WorkerGroup g(4, "test");
			std::vector<std::atomic<int>> hits(n);
			REQUIRE(g.kick(n, [&](size_t i) { hits[i].fetch_add(1); }));
			g.wait();
			for (size_t i = 0; i < n; ++i) {
				CHECK(hits[i].load() == 1);
			}
			CHECK(!g.busy());
		}
		{
			WorkerGroup g(6, "test");
			CHECK(g.lanes() == 6);
			std::atomic<uint64_t> total{ 0 };
			for (int k = 0; k < 1000; ++k) {
				REQUIRE(g.kick(64, [&](size_t i) { total.fetch_add(i + 1); }));
				g.wait();
			}
			CHECK(total.load() == 1000ull * (64ull * 65ull / 2ull));
		}
		{
			WorkerGroup g(2, "test");
			std::atomic<bool> release{ false };
			REQUIRE(g.kick(2, [&](size_t) {
				while (!release.load()) {
					std::this_thread::yield();
				}
			}));
			CHECK(g.busy());
			CHECK(!g.kick(1, [](size_t) {})); // refused while the job runs
			release = true;
			const double blocked = g.wait();
			CHECK(blocked >= 0.0);
			CHECK(!g.busy());
			CHECK(g.wait() == 0.0); // no job
		}
		{
			std::atomic<int> done{ 0 };
			{
				WorkerGroup g(3, "test");
				REQUIRE(g.kick(30, [&](size_t) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					done.fetch_add(1);
				}));
			} // the destructor waits for the job
			CHECK(done.load() == 30);
		}
		CHECK(WorkerGroup(0, "test").lanes() == 1);
		CHECK(WorkerGroup(1000, "test").lanes() == WorkerGroup::kMaxLanes);
	}

	TEST_CASE("s3 evaluator: lanes equal the single-thread run; LOD skips keep the front; layout; hostile inputs") {
		SynthCrowd c;
		const CrowdStageSpec spec;
		CrowdConfig config;
		config.temporal_lod = false;
		auto a = CrowdEvaluator::make(c.set, c.teams, spec.soldiers(), config);
		auto b = CrowdEvaluator::make(c.set, c.teams, spec.soldiers(), config);
		REQUIRE(a.ok());
		REQUIRE(b.ok());
		WorkerGroup lanes(6, "test");
		double t = 0.0;
		uint32_t two_cycle = 0, reloads = 0, aims = 0, evaluated = 0;
		for (int frame = 0; frame < 600; ++frame) {
			const double dt = 1.0 / 144.0 + 0.002 * (frame % 3);
			const std::vector<CrowdSoldierInput> in = stage_inputs(spec, t + dt, t, false);
			t += dt;
			(*a)->wait_and_publish(lanes);
			(*a)->set_inputs(in, dt);
			REQUIRE((*a)->kick(lanes));
			CHECK(!(*a)->kick(lanes)); // one job at a time
			(*b)->set_inputs(in, dt);
			(*b)->evaluate_all_here();
			(*a)->wait_and_publish(lanes);
			for (uint32_t i = 0; i < spec.soldiers(); ++i) {
				const std::span<const Mat34> pa = (*a)->palette(i), pb = (*b)->palette(i);
				REQUIRE(pa.size() == c.binds.size());
				REQUIRE(pa.size() == pb.size());
				CHECK(std::memcmp(pa.data(), pb.data(), pa.size_bytes()) == 0);
				CHECK((*a)->changed(i));
			}
			const CrowdFrameStats &s = (*a)->stats();
			two_cycle += s.two_cycle;
			reloads += s.reloading;
			aims += s.aiming;
			evaluated += s.evaluated;
			CHECK(s.evaluated == spec.soldiers());
			CHECK(s.skipped_lod == 0);
		}
		CHECK(evaluated == 600 * spec.soldiers());
		CHECK(two_cycle > evaluated / 5);
		CHECK(reloads > 0);
		CHECK(aims > 0);
		CHECK((*a)->bad_inputs() == 0);

		// Weapon layout: the masked joint takes the weapon pose; with an idle soldier the root and Hips do not.
		{
			std::vector<CrowdSoldierInput> idle(1);
			auto e = CrowdEvaluator::make(c.set, std::span<const CrowdTeam>(c.teams.data(), 1), 1, config);
			REQUIRE(e.ok());
			(*e)->set_inputs(idle, 0.01);
			(*e)->evaluate_all_here();
			const std::span<const Xform> m = (*e)->model(0);
			REQUIRE(m.size() == 4);
			const Xform expect = xform_mul(m[2], c.weapon_local[3]);
			CHECK(vec_dist(m[3].t, expect.t) < 1e-5);
			CHECK(quat_angle_deg(m[3].q, expect.q) < 1e-3);
			CHECK(vec_dist(m[0].t, Vec3{}) < 1e-6); // the root stays at rest: the sim owns it
		}

		// Temporal LOD: tiers by distance, staggered, skipped soldiers keep palette and model.
		{
			CrowdConfig lod;
			lod.temporal_lod = true;
			auto e = CrowdEvaluator::make(c.set, c.teams, spec.soldiers(), lod);
			REQUIRE(e.ok());
			std::vector<CrowdSoldierInput> in = stage_inputs(spec, 1.0, 1.0, true);
			const double dt = 1.0 / 144.0;
			(*e)->set_inputs(in, dt);
			(*e)->evaluate_all_here(); // first evaluation of everyone
			CHECK((*e)->stats().evaluated == spec.soldiers());
			std::array<uint64_t, TemporalLodTable::kTiers + 2> per_tier{};
			uint64_t skipped = 0;
			std::vector<std::vector<Mat34>> last(spec.soldiers());
			std::vector<std::vector<Xform>> last_model(spec.soldiers());
			uint32_t max_evaluated = 0;
			const int frames = 1440; // 10 s at 144 Hz
			double tt = 1.0;
			for (int f = 0; f < frames; ++f) {
				for (uint32_t i = 0; i < spec.soldiers(); ++i) {
					last[i].assign((*e)->palette(i).begin(), (*e)->palette(i).end());
					last_model[i].assign((*e)->model(i).begin(), (*e)->model(i).end());
				}
				in = stage_inputs(spec, tt + dt, tt, true);
				tt += dt;
				(*e)->set_inputs(in, dt);
				(*e)->evaluate_all_here();
				const CrowdFrameStats &s = (*e)->stats();
				max_evaluated = std::max(max_evaluated, s.evaluated);
				skipped += s.skipped_lod;
				for (size_t k = 0; k < per_tier.size(); ++k) {
					per_tier[k] += s.evaluated_by_tier[k];
				}
				for (uint32_t i = 0; i < spec.soldiers(); ++i) {
					if (!(*e)->changed(i)) {
						const std::span<const Mat34> p = (*e)->palette(i);
						REQUIRE(p.size() == last[i].size());
						CHECK(std::memcmp(p.data(), last[i].data(), p.size_bytes()) == 0);
						CHECK(std::memcmp((*e)->model(i).data(), last_model[i].data(), last_model[i].size() * sizeof(Xform)) == 0);
					}
				}
			}
			// Rows: 1 m (every frame), 15 m (0.02 s), 45 (0.04), 75 (0.06), 105 (0.08), 135 (0.10), 175 (0.12), 215 (frozen).
			const double seconds = frames * dt;
			const double per_row = spec.cols;
			CHECK(per_tier[0] == uint64_t(frames * per_row)); // nearer than First: every frame
			const double expect[6] = { 50, 25, 16.7, 12.5, 10, 8.33 };
			for (int k = 0; k < 6; ++k) {
				const double rate = double(per_tier[size_t(k) + 1]) / seconds / per_row;
				CHECK(rate == doctest::Approx(expect[k]).epsilon(0.03));
			}
			CHECK(per_tier[7] == 0); // beyond far: frozen
			CHECK(skipped > 0);
			// Staggered: the busiest frame evaluates far fewer than all soldiers.
			CHECK(max_evaluated < spec.soldiers() - 16);
		}

		// Hostile inputs: non-finite values are replaced and counted; nothing becomes non-finite.
		{
			auto e = CrowdEvaluator::make(c.set, c.teams, 4, config);
			REQUIRE(e.ok());
			const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
			std::vector<CrowdSoldierInput> in(4);
			in[0].velocity = Vec3{ nan, 0, 0 };
			in[1].yaw = inf;
			in[2].pitch = nan;
			in[3].position = Vec3{ 0, -inf, 0 };
			in[3].camera_distance = nan;
			in[3].team = 200; // clamped to the last team
			(*e)->set_inputs(in, std::numeric_limits<double>::quiet_NaN());
			(*e)->evaluate_all_here();
			CHECK((*e)->bad_inputs() == 6);
			for (uint32_t i = 0; i < 4; ++i) {
				for (const Mat34 &m : (*e)->palette(i)) {
					for (const auto &row : m.m) {
						for (float v : row) {
							CHECK(std::isfinite(v));
						}
					}
				}
			}
			CHECK((*e)->palette(99).empty());
			CHECK(!(*e)->changed(99));
		}
		// make() refuses bad configurations.
		{
			CHECK(!CrowdEvaluator::make(c.set, c.teams, kMaxCrowdSoldiers + 1, config).ok());
			CHECK(!CrowdEvaluator::make(c.set, {}, 4, config).ok());
			CrowdTeam bad = c.teams[0];
			bad.binds = nullptr;
			CHECK(!CrowdEvaluator::make(c.set, std::span<const CrowdTeam>(&bad, 1), 4, config).ok());
			BoneMask short_mask;
			short_mask.weight = { 1 };
			bad = c.teams[0];
			bad.weapon_joints = &short_mask;
			CHECK(!CrowdEvaluator::make(c.set, std::span<const CrowdTeam>(&bad, 1), 4, config).ok());
		}
	}

	TEST_CASE("s3 stage: every phase at every time, continuous ramps, reload starts, coverage above the G0 floors") {
		const CrowdStageSpec spec;
		const double period = spec.phase_seconds * double(kStagePhases);
		// Every phase is on screen at every sampled time.
		for (double t = 0.0; t < period; t += 0.37) {
			std::array<int, kStagePhases> seen{};
			for (uint32_t i = 0; i < spec.soldiers(); ++i) {
				seen[size_t(stage_phase(spec, i, t))]++;
			}
			for (size_t k = 0; k < kStagePhases; ++k) {
				CHECK(seen[k] > 0);
			}
		}
		// Teams: columns 0-3 US, 4-7 RU.
		uint32_t us = 0;
		for (uint32_t i = 0; i < spec.soldiers(); ++i) {
			const CrowdSoldierInput in = stage_input(spec, i, 0.0, 0.0, Vec3{}, false);
			us += in.team == 0 ? 1 : 0;
			CHECK(in.team == ((i % spec.cols) < 4 ? 0 : 1));
		}
		CHECK(us == 32);
		// Continuous: at 144 Hz the local velocity never jumps by more than the ramp allows (a full 4 -> -3 m/s swing
		// over 1 s is 7 m/s^2), and the facing never jumps.
		const double dt = 1.0 / 144.0;
		for (uint32_t i : { 0u, 17u, 63u }) {
			CrowdSoldierInput prev = stage_input(spec, i, 0.0, 0.0, Vec3{}, false);
			uint32_t reload_starts = 0;
			for (double t = dt; t < period * 2.0; t += dt) {
				const CrowdSoldierInput in = stage_input(spec, i, t, t - dt, Vec3{}, false);
				const float dv = float(vec_dist(in.velocity, prev.velocity));
				// A ramp swings up to 7 m/s over 1 s; at RunCircle's start the target itself turns (3.5 m/s at 90 deg/s).
				CHECK(dv < 12.0f * float(dt) + 1e-3f);
				const float dyaw = std::fabs(std::remainder(in.yaw - prev.yaw, 2.0f * kPi));
				CHECK(dyaw < 0.03f);
				CHECK(std::fabs(in.pitch - prev.pitch) < 1.6f * float(dt) + 1e-4f); // AimUp -> AimDown: 90 deg over the 1 s ramp
				reload_starts += (in.flags & crowd_flags::kReloadStart) != 0 ? 1 : 0;
				prev = in;
			}
			CHECK(reload_starts == 2); // once per 40 s period
		}
		// LOD rows.
		for (uint32_t i = 0; i < spec.soldiers(); ++i) {
			const CrowdSoldierInput in = stage_input(spec, i, 0.0, 0.0, Vec3{}, true);
			CHECK(in.camera_distance == kLodRowDistance[i / spec.cols]);
		}
		// Real camera distances 10..25 m.
		for (uint32_t i = 0; i < spec.soldiers(); ++i) {
			const CrowdSoldierInput in = stage_input(spec, i, 0.0, 0.0, stage_camera(spec).position, false);
			CHECK(in.camera_distance > 9.0f);
			CHECK(in.camera_distance < 26.0f);
		}
		// Blend coverage with the retail-like cycle directions.
		std::array<LocoClipInfo, kLocoClips> clips{};
		clips[size_t(LocoClip::Fwd)] = LocoClipInfo{ 0.6f, 4.64f, Vec3{ 0, 0, 1 } };
		clips[size_t(LocoClip::Bwd)] = LocoClipInfo{ 0.567f, 2.97f, Vec3{ 0, 0, -1 } };
		clips[size_t(LocoClip::Left)] = LocoClipInfo{ 0.567f, 2.6f, Vec3{ 1, 0, 0 } };
		clips[size_t(LocoClip::Right)] = LocoClipInfo{ 0.533f, 3.2f, Vec3{ -1, 0, 0 } };
		const StageCoverage cov = stage_coverage(spec, clips, LocoParams{}, 0.05);
		MESSAGE("stage coverage: two-cycle " << cov.two_cycle_fraction << ", idle mix " << cov.idle_mix_fraction << ", clips/soldier " << cov.mean_loco_clips);
		CHECK(cov.two_cycle_fraction > s3_gate::kMinTwoCycleFraction);
		CHECK(cov.idle_mix_fraction > s3_gate::kMinIdleMixFraction);
		CHECK(cov.mean_loco_clips > s3_gate::kMinMeanLocoClips);
		// Hostile step: nothing.
		CHECK(stage_coverage(spec, clips, LocoParams{}, 0.0).mean_loco_clips == 0.0);
		CHECK(stage_coverage(spec, clips, LocoParams{}, std::numeric_limits<double>::quiet_NaN()).mean_loco_clips == 0.0);
		// Non-finite time: a valid input.
		const CrowdSoldierInput in = stage_input(spec, 3, std::numeric_limits<double>::infinity(), 0.0, Vec3{}, false);
		CHECK(std::isfinite(in.yaw));
		CHECK(std::isfinite(in.velocity.x));
	}
}

// ---- install ------------------------------------------------------------------------------------------------------

namespace {

struct S3Data {
	bool ok = false;
	std::string why;
	bf3::core::InstallReader reader;
	bf3::core::InstallSnapshot before;
	std::shared_ptr<bf3::core::Install> install;
	std::unique_ptr<bf3::core::ant::AntInstallSource> source;
	std::unique_ptr<bf3::core::ant::AntLibrary> library;
	CrowdClipSet clips;
	double seconds_clips = 0.0;
};

S3Data *s3_data() {
	static S3Data *data = [] {
		auto *d = new S3Data();
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
		auto lib = load_ant_library(*d->source);
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? "" : lib.error().message));
		d->library = std::move(*lib);
		const auto t0 = std::chrono::steady_clock::now();
		auto clips = load_crowd_clips(*d->source, *d->library);
		d->seconds_clips = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		REQUIRE_MESSAGE(clips.ok(), (clips.ok() ? "" : clips.error().message));
		d->clips = std::move(*clips);
		d->ok = true;
		return d;
	}();
	return data;
}

S3Data *s3_or_skip() {
	S3Data *d = s3_data();
	if (!d->ok) {
		bf3::test::skip_install_case(d->why.c_str());
		return nullptr;
	}
	return d;
}

bool is_leg_or_hips(const std::string &p_name) {
	for (const char *k : { "Hips", "Leg", "Foot", "Toe", "Knee", "Ankle" }) {
		if (p_name.find(k) != std::string::npos) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_SUITE("anim-s3") {
	TEST_CASE("s3 clips: the eight controllers, frames, channels, root motion") {
		S3Data *d = s3_or_skip();
		if (d == nullptr) {
			return;
		}
		const CrowdClipSet &s = d->clips;
		MESSAGE("load_crowd_clips: " << d->seconds_clips << " s");
		CHECK(s.rig.size() == 215);
		CHECK(s.package->object_count() == 2444);
		const uint32_t end_frames[kCrowdClips] = { 1, 18, 17, 17, 16, 1, 1, 40 };
		for (size_t k = 0; k < kCrowdClips; ++k) {
			const CrowdClipData &c = s.clips[k];
			REQUIRE(c.binding.rig != nullptr);
			CHECK(c.tracks.end_frame == end_frames[k]);
			CHECK(c.clip.end_frame == end_frames[k]);
			CHECK(c.mask.size() == s.rig.size());
		}
		for (const CrowdClip k : { CrowdClip::Idle, CrowdClip::RunFwd, CrowdClip::RunBwd, CrowdClip::RunLeft, CrowdClip::RunRight }) {
			const CrowdClipData &c = s.get(k);
			CHECK(c.clip.num_quats == 93);
			CHECK(c.clip.num_vec3s == 22);
			CHECK(c.clip.num_floats == 8);
			CHECK(!c.additive);
		}
		for (const CrowdClip k : { CrowdClip::AimUp, CrowdClip::AimDown }) {
			CHECK(s.get(k).clip.num_quats == 41);
			CHECK(s.get(k).clip.num_vec3s == 12);
			CHECK(s.get(k).additive);
		}
		CHECK(s.get(CrowdClip::RunNoise).additive);
		CHECK(s.get(CrowdClip::RunNoise).clip.num_quats == 112);
		const double speeds[4] = { 2.784 / 0.6, 1.686 / (17.0 / 30.0), 1.472 / (17.0 / 30.0), 1.71 / (16.0 / 30.0) };
		for (size_t k = 1; k < kLocoClips; ++k) {
			MESSAGE("loco " << k << ": duration " << s.loco[k].duration << " s, root speed " << s.loco[k].root_speed << " m/s, dir (" << s.loco[k].root_dir.x << ", "
							<< s.loco[k].root_dir.z << ")");
			CHECK(s.loco[k].root_speed == doctest::Approx(speeds[k - 1]).epsilon(0.01));
		}
		CHECK(s.loco[size_t(LocoClip::Fwd)].root_dir.z > 0.99f);
		CHECK(s.loco[size_t(LocoClip::Bwd)].root_dir.z < -0.99f);
		CHECK(s.loco[size_t(LocoClip::Left)].root_dir.x > 0.99f);
		CHECK(s.loco[size_t(LocoClip::Right)].root_dir.x < -0.99f);
		CHECK(s.root_joint == s.rig.find("AITrajectory"));
		// The Wep_* joints the idle, run and aim clips leave undriven.
		BoneMask driven;
		for (const CrowdClip k : { CrowdClip::Idle, CrowdClip::RunFwd, CrowdClip::RunBwd, CrowdClip::RunLeft, CrowdClip::RunRight, CrowdClip::AimUp, CrowdClip::AimDown }) {
			driven = mask_union(driven, s.get(k).mask);
		}
		uint32_t undriven = 0;
		for (size_t j = 0; j < s.rig.size(); ++j) {
			if (s.rig.bone(j).name.rfind("Wep_", 0) == 0 && driven.weight[j] == 0.0f) {
				++undriven;
			}
		}
		CHECK(driven.weight[size_t(s.rig.find("Wep_Root"))] > 0.0f);
		CHECK(undriven == 21);
		// A name that does not occur once is refused.
		CrowdClipNames bad;
		bad.controllers[size_t(CrowdClip::RunFwd)] = "Reload_NoAddon_Stand"; // 76 copies
		auto r = load_crowd_clips(*d->source, *d->library, bad);
		REQUIRE(!r.ok());
		CHECK(r.error().message.find("76 times") != std::string::npos);
		bad.controllers[size_t(CrowdClip::RunFwd)] = "No_Such_Controller";
		CHECK(!load_crowd_clips(*d->source, *d->library, bad).ok());
	}

	TEST_CASE("s3 reload: weapon -> chooser index -> clip, the upper-body mask") {
		S3Data *d = s3_or_skip();
		if (d == nullptr) {
			return;
		}
		const struct {
			uint32_t ant_ref;
			uint32_t index;
			uint32_t controller;
			uint32_t frames;
		} weapons[] = { { 357063601u, 41, 0x830e1, 95 }, { 357087606u, 33, 0x8cb0b, 108 } };
		for (const auto &w : weapons) {
			auto index = weapon_choice_index(*d->library, w.ant_ref);
			REQUIRE_MESSAGE(index.ok(), (index.ok() ? "" : index.error().message));
			CHECK(*index == w.index);
			auto clip = load_chosen_clip(d->clips, *d->library, CrowdClipNames{}.reload_chooser, *index);
			REQUIRE_MESSAGE(clip.ok(), (clip.ok() ? "" : clip.error().message));
			CHECK((clip->controller_index & 0xFFFFFu) == w.controller);
			CHECK(clip->tracks.end_frame == w.frames);
			CHECK(clip->clip.num_quats == 94);
			CHECK(clip->clip.num_vec3s == 37);
			uint32_t legs = 0, upper = 0;
			for (size_t j = 0; j < d->clips.rig.size(); ++j) {
				if (clip->mask.weight[j] > 0.0f) {
					(is_leg_or_hips(d->clips.rig.bone(j).name) ? legs : upper)++;
				}
			}
			CHECK(legs == 0);
			CHECK(upper > 60);
			for (const char *n : { "Spine", "Spine2", "LeftArm", "LeftArmRoll", "RightForeArm", "Wep_Root", "Wep_Mag" }) {
				CHECK_MESSAGE(clip->mask.weight[size_t(d->clips.rig.find(n))] > 0.0f, n);
			}
		}
		// Hostile lookups: errors, never a crash or a wrong success.
		std::mt19937 rng(1234);
		uint32_t errors = 0, ok = 0;
		for (int k = 0; k < 200; ++k) {
			const uint32_t id = k < 100 ? uint32_t(rng()) : (0x15480000u | (rng() & 0xFFFFu));
			auto r = weapon_choice_index(*d->library, id);
			(r.ok() ? ok : errors)++;
			if (r.ok()) {
				CHECK(*r < 95);
			}
		}
		MESSAGE("random AntRefs: " << errors << " errors, " << ok << " resolved");
		CHECK(errors > 100);
		for (const uint32_t bad : { 95u, 96u, 1000u, 0xFFFFFFFFu }) {
			CHECK(!load_chosen_clip(d->clips, *d->library, CrowdClipNames{}.reload_chooser, bad).ok());
		}
		CHECK(!load_chosen_clip(d->clips, *d->library, "No_Such_Chooser", 0).ok());
		uint32_t resolved = 0;
		for (uint32_t i = 0; i < 95; ++i) {
			auto r = load_chosen_clip(d->clips, *d->library, CrowdClipNames{}.reload_chooser, i);
			resolved += r.ok() ? 1 : 0;
		}
		MESSAGE("chooser entries that resolve to a clip of b_basicassetsmp: " << resolved << " / 95");
		CHECK(resolved >= 76);
	}

	TEST_CASE("s3 additive order: the weapon-pitch criterion of s3.md 5.5") {
		S3Data *d = s3_or_skip();
		if (d == nullptr) {
			return;
		}
		bool qualifies[2] = {};
		AimMeasure post{};
		for (int order = 0; order < 2; ++order) {
			const AdditiveOrder o = order == 0 ? AdditiveOrder::LocalPost : AdditiveOrder::LocalPre;
			auto m = measure_aim(d->clips, o);
			REQUIRE_MESSAGE(m.ok(), (m.ok() ? "" : m.error().message));
			const std::string name = order == 0 ? "LocalPost" : "LocalPre";
			MESSAGE(name << ": barrel = Wep_Root axis " << m->barrel_axis << (m->barrel_negative ? std::string(" (negative)") : std::string()) << "; AimUp "
						 << m->up_deg << " deg (azimuth " << m->up_azimuth_deg << "), AimDown " << m->down_deg << " deg (azimuth " << m->down_azimuth_deg
						 << "), monotonic " << int(m->monotonic) << (m->qualifies() ? std::string(" -> qualifies") : std::string()));
			qualifies[order] = m->qualifies();
			if (order == 0) {
				post = *m;
			}
		}
		const AdditiveOrder chosen = qualifies[0] ? AdditiveOrder::LocalPost : AdditiveOrder::LocalPre;
		MESSAGE("additive order chosen: " << (chosen == AdditiveOrder::LocalPost ? std::string("LocalPost") : std::string("LocalPre")));
		CHECK((qualifies[0] || qualifies[1]));
		const CrowdConfig defaults;
		CHECK(defaults.aim_order == chosen); // the default is the measured choice
		// The default full-aim pitches are the measured barrel elevations (so the barrel follows the pitch).
		CHECK(defaults.aim_up_full_pitch_deg == doctest::Approx(post.up_deg).epsilon(0.01));
		CHECK(defaults.aim_down_full_pitch_deg == doctest::Approx(-post.down_deg).epsilon(0.01));
	}

	TEST_CASE("s3 bench: 64 soldiers, single thread and on lanes (reported)") {
		S3Data *d = s3_or_skip();
		if (d == nullptr) {
			return;
		}
		// Binds: every rig joint against its own rest (the cost is the same as the meshes' 108-113 binds).
		EbxSkeleton ebx;
		ebx.partition = "rig";
		std::vector<Xform> rest;
		model_pose(d->clips.rig, rest_pose(d->clips.rig), rest);
		for (size_t i = 0; i < d->clips.rig.size(); ++i) {
			ebx.bone_names.push_back(d->clips.rig.bone(i).name);
			ebx.parents.push_back(d->clips.rig.bone(i).parent);
			ebx.local_pose.push_back(mat_from_xform(d->clips.rig.bone(i).rest));
			ebx.model_pose.push_back(mat_from_xform(rest[i]));
		}
		SkinBinds binds;
		for (uint32_t i = 0; i < 113; ++i) {
			REQUIRE(binds.bind(ebx, i, d->clips.rig).ok());
		}
		auto reload = load_chosen_clip(d->clips, *d->library, CrowdClipNames{}.reload_chooser, 41);
		REQUIRE(reload.ok());
		CrowdTeam team;
		team.binds = &binds;
		team.reload = &*reload;
		const std::vector<CrowdTeam> teams{ team, team };
		CrowdConfig config;
		config.temporal_lod = false;
		const CrowdStageSpec spec;
		auto single = CrowdEvaluator::make(d->clips, teams, spec.soldiers(), config);
		auto laned = CrowdEvaluator::make(d->clips, teams, spec.soldiers(), config);
		REQUIRE(single.ok());
		REQUIRE(laned.ok());
		const unsigned hc = std::thread::hardware_concurrency();
		const unsigned lanes_n = std::clamp(hc == 0 ? 2u : (hc > 4 ? hc - 4 : 0u), 2u, 6u);
		WorkerGroup lanes(lanes_n, "bench");
		const double dt = 1.0 / 144.0;
		double t = 0.0, single_s = 0.0, wall_s = 0.0, job_s = 0.0;
		const int frames = 600;
		uint64_t clips_sampled = 0;
		for (int f = 0; f < frames; ++f) {
			const std::vector<CrowdSoldierInput> in = stage_inputs(spec, t + dt, t, false);
			t += dt;
			auto t0 = std::chrono::steady_clock::now();
			(*single)->set_inputs(in, dt);
			(*single)->evaluate_all_here();
			auto t1 = std::chrono::steady_clock::now();
			(*laned)->set_inputs(in, dt);
			REQUIRE((*laned)->kick(lanes));
			(*laned)->wait_and_publish(lanes);
			auto t2 = std::chrono::steady_clock::now();
			single_s += std::chrono::duration<double>(t1 - t0).count();
			wall_s += std::chrono::duration<double>(t2 - t1).count();
			job_s += (*laned)->stats().job_seconds;
			clips_sampled += (*laned)->stats().loco_clips_sampled;
		}
		MESSAGE("bench (shared machine, noisy): 64 soldiers x " << d->clips.rig.size() << " joints, 113 binds: single thread " << single_s / frames * 1000.0
															 << " ms/frame; " << lanes_n << " lanes " << wall_s / frames * 1000.0 << " ms/frame wall (job " << job_s / frames * 1000.0
															 << " ms); " << double(clips_sampled) / (frames * 64.0) << " locomotion clips per soldier-frame");
		CHECK(single_s > 0.0);
		const auto after = d->reader.snapshot();
		const bf3::core::SnapshotDiff diff = bf3::core::diff_snapshots(d->before, after);
		MESSAGE("install unchanged: " << (diff.clean() ? "yes" : diff.summary()));
		CHECK(diff.clean());
	}
}
