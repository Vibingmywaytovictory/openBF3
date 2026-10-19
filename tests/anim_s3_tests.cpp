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

// ---- M2a tp (docs/design/m2a.md §9, R5, E18 (b); docs/design/m2a-tp.md) ----------------------------------------------
// anim-s3-unit (synthetic, no data):
//   * R5 with one weapon per team (the team's own binds, reload and layout as item 0) equals S3's evaluator bit for bit
//     (palettes and models, on lanes and single-threaded, LOD off and on), and the weapon palette is the team's;
//   * two weapons per team, teams with different bind counts: weapon_of, the weapon palette, the per-weapon layout and
//     reload, a weapon change cancelling a reload, item 0 for an index past the end, muzzles, make()'s refusals;
//   * the stance set: stand bit for bit, crouch / prone idles, a transition at its fraction and its stand-in lerp, the
//     moves, fire (kick and crouch clip), death (Q13 blend, the clip style, hidden after 3 s, respawn), hostile values.
// anim-s3 (the install): E18 (b), the crouch and prone rows of lc's capsule pose table against CrowdEvaluator with the
// stance set (gate 3 cm, head and chest), the transitions reported; the move clips and the stance set on the retail rig.

#include "bf3/anim/server_pose.h"

namespace {

bool palettes_equal(std::span<const Mat34> p_a, std::span<const Mat34> p_b) {
	return p_a.size() == p_b.size() && std::memcmp(p_a.data(), p_b.data(), p_a.size_bytes()) == 0;
}

bool models_equal(std::span<const Xform> p_a, std::span<const Xform> p_b) {
	return p_a.size() == p_b.size() && std::memcmp(p_a.data(), p_b.data(), p_a.size_bytes()) == 0;
}

// SynthCrowd plus R5's items: weapon 0 = the team's own data; weapon 1 binds only Arm and Wep, a shorter reload and
// another layout; a second team whose body binds leave out Wep (3 binds instead of 4).
struct SynthWeapons {
	SynthCrowd c;
	SkinBinds binds_w1, binds_body_b;
	CrowdClipData reload_w1;
	std::vector<Xform> local_w1;
	std::array<CrowdWeapon, 2> weapons{};
	std::array<CrowdWeapon, 1> one{};
	std::vector<CrowdTeam> teams_one, teams_two;
	Xform muzzle_offset{ axis_angle(1, 0, 0, 90), Vec3{ 0, 0, 0.5f } };

	SynthWeapons() {
		const Skeleton &r = c.set.rig;
		EbxSkeleton ebx;
		ebx.partition = "synthetic-weapon";
		std::vector<Xform> model;
		model_pose(r, rest_pose(r), model);
		for (size_t i = 0; i < r.size(); ++i) {
			ebx.bone_names.push_back(r.bone(i).name);
			ebx.parents.push_back(r.bone(i).parent);
			ebx.local_pose.push_back(mat_from_xform(r.bone(i).rest));
			ebx.model_pose.push_back(mat_from_xform(model[i]));
		}
		REQUIRE(binds_w1.bind(ebx, 2, r).ok());
		REQUIRE(binds_w1.bind(ebx, 3, r).ok());
		for (uint32_t i = 0; i < 3; ++i) {
			REQUIRE(binds_body_b.bind(ebx, i, r).ok());
		}
		reload_w1 = synth_clip(r, 10, { 2, 3 }, axis_angle(1, 0, 0, 40), { 3 }, Vec3{ 0.3f, 0, 0 }, Vec3{ 0.3f, -0.1f, 0 });
		local_w1.assign(r.size(), xform_identity());
		local_w1[3] = Xform{ axis_angle(0, 0, 1, 45), Vec3{ 0.1f, 0.2f, 0 } };
		CrowdWeapon w0;
		w0.binds = &c.binds;
		w0.reload = &c.reload;
		w0.weapon_joints = &c.weapon_joints;
		w0.weapon_local = c.weapon_local;
		w0.muzzle_joint = 3;
		w0.muzzle_offset = muzzle_offset;
		CrowdWeapon w1;
		w1.binds = &binds_w1;
		w1.reload = &reload_w1;
		w1.reload_rate = 2.0f;
		w1.weapon_joints = &c.weapon_joints;
		w1.weapon_local = local_w1;
		weapons = { w0, w1 };
		one = { w0 };
		teams_one = c.teams;
		for (CrowdTeam &t : teams_one) {
			t.weapons = one;
		}
		teams_two = c.teams;
		teams_two[0].weapons = weapons;
		teams_two[1].binds = &binds_body_b;
		teams_two[1].weapons = weapons;
	}
};

// A synthetic stance set on SynthCrowd's rig: crouch idle Hips at 0.6 m, prone idle at 0.2 m, StandToCrouch moves the
// Hips 1.0 -> 0.6 over 10 frames, the other transitions absent; the aims are the stand aims. Moves: crouch cycles =
// the run cycles with a smaller swing, prone absent. Crouch fire additive on Arm; deaths Hips 1.0 -> 0.1 over 15 frames.
struct SynthStance {
	ServerPoseClips pose;
	CrowdMoveClips moves;
	CrowdStanceSet set;

	explicit SynthStance(const SynthCrowd &p_c, CrowdDeathStyle p_death = kCrowdDeathDefault) {
		const Skeleton &r = p_c.set.rig;
		pose.rig = r;
		pose.root_joint = 0;
		auto put = [&](ServerPoseClip p_role, CrowdClipData p_clip) {
			pose.clips[size_t(p_role)] = std::move(p_clip);
			pose.present[size_t(p_role)] = true;
		};
		put(ServerPoseClip::StandIdle, p_c.set.get(CrowdClip::Idle));
		put(ServerPoseClip::StandAimUp, p_c.set.get(CrowdClip::AimUp));
		put(ServerPoseClip::StandAimDown, p_c.set.get(CrowdClip::AimDown));
		put(ServerPoseClip::CrouchIdle, synth_clip(r, 1, { 1 }, Quat{}, { 1 }, Vec3{ 0, 0.6f, 0 }, Vec3{ 0, 0.6f, 0 }));
		put(ServerPoseClip::CrouchAimUp, p_c.set.get(CrowdClip::AimUp));
		put(ServerPoseClip::CrouchAimDown, p_c.set.get(CrowdClip::AimDown));
		put(ServerPoseClip::ProneIdle, synth_clip(r, 1, { 1 }, Quat{}, { 1 }, Vec3{ 0, 0.2f, 0 }, Vec3{ 0, 0.2f, 0 }));
		put(ServerPoseClip::ProneAimUp, p_c.set.get(CrowdClip::AimUp));
		put(ServerPoseClip::ProneAimDown, p_c.set.get(CrowdClip::AimDown));
		put(ServerPoseClip::StandToCrouch, synth_clip(r, 10, { 1 }, Quat{}, { 1 }, Vec3{ 0, 1.0f, 0 }, Vec3{ 0, 0.6f, 0 }));
		static constexpr CrowdMoveClip kCrouch[4] = { CrowdMoveClip::SneakFwd, CrowdMoveClip::SneakBwdLeft, CrowdMoveClip::SneakLeft, CrowdMoveClip::SneakRight };
		static constexpr CrowdClip kRun[4] = { CrowdClip::RunFwd, CrowdClip::RunBwd, CrowdClip::RunLeft, CrowdClip::RunRight };
		for (int k = 0; k < 4; ++k) {
			moves.clips[size_t(kCrouch[k])] = p_c.set.get(kRun[k]);
			moves.present[size_t(kCrouch[k])] = true;
			moves.loco[0][size_t(k) + 1] = p_c.set.loco[size_t(k) + 1];
			moves.loco[0][size_t(k) + 1].root_speed *= 0.5f;
		}
		moves.clips[size_t(CrowdMoveClip::SneakBwdRight)] = p_c.set.get(CrowdClip::RunBwd);
		moves.present[size_t(CrowdMoveClip::SneakBwdRight)] = true;
		moves.moves_ok = { true, false };
		moves.clips[size_t(CrowdMoveClip::CrouchFire)] = synth_clip(r, 21, { 2 }, axis_angle(0, 0, 1, 10), {}, Vec3{}, Vec3{}, true);
		moves.present[size_t(CrowdMoveClip::CrouchFire)] = true;
		for (const CrowdMoveClip d : { CrowdMoveClip::DeathStand, CrowdMoveClip::DeathCrouch, CrowdMoveClip::DeathProne }) {
			moves.clips[size_t(d)] = synth_clip(r, 15, { 1 }, axis_angle(1, 0, 0, 80), { 1 }, Vec3{ 0, 1.0f, 0 }, Vec3{ 0, 0.1f, 0 });
			moves.present[size_t(d)] = true;
		}
		set.pose = &pose;
		set.moves = &moves;
		set.death = p_death;
	}
};

float hips_y(const CrowdEvaluator &p_e, uint32_t p_i) {
	return p_e.model(p_i)[1].t.y;
}

} // namespace

TEST_SUITE("anim-s3-unit") {
	TEST_CASE("M2a tp R5: one weapon per team equals S3's evaluator bit for bit; the weapon palette is the team's") {
		SynthWeapons w;
		const CrowdStageSpec spec;
		for (const bool lod : { false, true }) {
			CrowdConfig config;
			config.temporal_lod = lod;
			auto s3 = CrowdEvaluator::make(w.c.set, w.c.teams, spec.soldiers(), config);
			auto r5 = CrowdEvaluator::make(w.c.set, w.teams_one, spec.soldiers(), config);
			auto r5_lanes = CrowdEvaluator::make(w.c.set, w.teams_one, spec.soldiers(), config);
			REQUIRE(s3.ok());
			REQUIRE(r5.ok());
			REQUIRE(r5_lanes.ok());
			WorkerGroup lanes(4, "test");
			double t = 0.0;
			uint64_t compared = 0, reloading = 0, changed = 0;
			for (int frame = 0; frame < 480; ++frame) {
				const double dt = 1.0 / 144.0 + 0.002 * (frame % 3);
				const std::vector<CrowdSoldierInput> in = stage_inputs(spec, t + dt, t, lod);
				t += dt;
				(*s3)->set_inputs(in, dt);
				(*s3)->evaluate_all_here();
				(*r5)->set_inputs(in, dt);
				(*r5)->evaluate_all_here();
				(*r5_lanes)->set_inputs(in, dt);
				REQUIRE((*r5_lanes)->kick(lanes));
				(*r5_lanes)->wait_and_publish(lanes);
				reloading += (*r5)->stats().reloading;
				for (uint32_t i = 0; i < spec.soldiers(); ++i) {
					REQUIRE((*s3)->changed(i) == (*r5)->changed(i));
					changed += (*r5)->changed(i) ? 1 : 0;
					CHECK(palettes_equal((*s3)->palette(i), (*r5)->palette(i)));
					CHECK(models_equal((*s3)->model(i), (*r5)->model(i)));
					CHECK(palettes_equal((*r5)->palette(i), (*r5_lanes)->palette(i)));
					CHECK((*r5)->weapon_of(i) == 0);
					CHECK(palettes_equal((*r5)->weapon_palette(i), (*r5)->palette(i))); // item 0 = the team's binds
					Xform mz;
					REQUIRE((*r5)->muzzle_model(i, mz));
					const Xform expect = xform_mul((*r5)->model(i)[3], w.muzzle_offset);
					CHECK(std::memcmp(&mz, &expect, sizeof(Xform)) == 0);
					CHECK((*s3)->weapon_of(i) == kNoCrowdWeapon);
					CHECK((*s3)->weapon_palette(i).empty());
					CHECK(!(*s3)->muzzle_model(i, mz));
					++compared;
				}
				const CrowdFrameStats &a = (*s3)->stats(), &b = (*r5)->stats();
				CHECK(a.evaluated == b.evaluated);
				CHECK(a.skipped_lod == b.skipped_lod);
				CHECK(a.loco_clips_sampled == b.loco_clips_sampled);
				CHECK(a.reloading == b.reloading);
				CHECK(a.aiming == b.aiming);
			}
			MESSAGE("R5 one weapon per team, LOD " << std::string(lod ? "on" : "off") << ": " << compared << " soldier-frames bit-equal to S3, " << changed << " changed, "
												   << reloading << " reloading");
			CHECK(reloading > 0);
			CHECK((*r5)->m2a_stats().bad_weapon == 0);
		}
	}

	TEST_CASE("M2a tp R5: two weapons per team, teams with different bind counts, reloads, muzzles, refusals") {
		SynthWeapons w;
		CrowdConfig config;
		config.temporal_lod = false;
		auto e = CrowdEvaluator::make(w.c.set, w.teams_two, 6, config);
		REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
		CrowdEvaluator &ev = **e;
		// Before any evaluation: empty palettes (R5's binds-init fix), no weapon.
		for (uint32_t i = 0; i < 6; ++i) {
			CHECK(ev.palette(i).empty());
			CHECK(ev.weapon_of(i) == kNoCrowdWeapon);
			CHECK(!ev.drawn(i));
		}
		std::vector<CrowdSoldierInput> in(6);
		in[0].team = 0;
		in[0].weapon = 0;
		in[1].team = 0;
		in[1].weapon = 1;
		in[2].team = 1;
		in[2].weapon = 1;
		in[3].team = 1;
		in[3].weapon = 9; // past the end: item 0, counted
		in[4].team = 0;
		in[4].weapon = 0;
		in[5].team = 1;
		in[5].flags = crowd_flags::kHidden;
		ev.set_inputs(in, 1.0 / 60.0);
		ev.evaluate_all_here();
		CHECK(ev.palette(0).size() == 4);
		CHECK(ev.palette(1).size() == 4);
		CHECK(ev.palette(2).size() == 3); // team 1's body binds
		CHECK(ev.palette(3).size() == 3);
		CHECK(ev.weapon_of(0) == 0);
		CHECK(ev.weapon_of(1) == 1);
		CHECK(ev.weapon_of(2) == 1);
		CHECK(ev.weapon_of(3) == 0);
		CHECK(ev.weapon_of(5) == kNoCrowdWeapon);
		CHECK(!ev.drawn(5));
		CHECK(ev.weapon_palette(0).size() == 4);
		CHECK(ev.weapon_palette(1).size() == 2);
		CHECK(ev.weapon_palette(2).size() == 2);
		CHECK(ev.weapon_palette(3).size() == 4);
		CHECK(ev.m2a_stats().bad_weapon == 1);
		CHECK(ev.m2a_stats().weapon_palettes == 5);
		// The weapon palette of item 1 is the skin of Arm and Wep in the same model pose.
		{
			std::vector<Mat34> expect;
			std::vector<Xform> model(ev.model(1).begin(), ev.model(1).end());
			skin_matrices(model, w.binds_w1.binds(), expect);
			CHECK(palettes_equal(ev.weapon_palette(1), expect));
		}
		// Per-weapon layout of step 3: Wep takes item 1's LocalPose.
		{
			const Xform expect1 = xform_mul(ev.model(1)[2], w.local_w1[3]);
			CHECK(vec_dist(ev.model(1)[3].t, expect1.t) < 1e-5);
			const Xform expect0 = xform_mul(ev.model(0)[2], w.c.weapon_local[3]);
			CHECK(vec_dist(ev.model(0)[3].t, expect0.t) < 1e-5);
			CHECK(vec_dist(ev.model(0)[3].t, ev.model(1)[3].t) > 0.05);
		}
		// Muzzles: item 0 hangs on Wep, item 1 has none.
		Xform mz;
		CHECK(ev.muzzle_model(0, mz));
		CHECK(!ev.muzzle_model(1, mz));
		CHECK(ev.m2a_stats().muzzles == 3); // soldiers 0, 3, 4
		// Reloads: item 1's clip is 10 frames at rate 2 (0.167 s), item 0's 30 frames (1.0 s).
		in[0].flags = crowd_flags::kReloadStart;
		in[1].flags = crowd_flags::kReloadStart;
		in[4].flags = crowd_flags::kReloadStart;
		ev.set_inputs(in, 1.0 / 60.0);
		ev.evaluate_all_here();
		in[0].flags = in[1].flags = in[4].flags = 0;
		CHECK(ev.stats().reloading == 0); // the first reload frame has weight 0 (the ramp)
		for (int f = 0; f < 18; ++f) { // 0.3 s
			ev.set_inputs(in, 1.0 / 60.0);
			ev.evaluate_all_here();
		}
		CHECK(ev.stats().reloading == 2); // soldiers 0 and 4; item 1's reload is over
		// A weapon change during the reload cancels it.
		in[4].weapon = 1;
		ev.set_inputs(in, 1.0 / 60.0);
		ev.evaluate_all_here();
		CHECK(ev.stats().reloading == 1);
		CHECK(ev.m2a_stats().reloads_cancelled == 1);
		CHECK(ev.weapon_of(4) == 1);
		CHECK(ev.weapon_palette(4).size() == 2);
		// make() refuses bad items.
		{
			std::array<CrowdWeapon, kMaxCrowdWeapons + 1> many;
			many.fill(w.weapons[0]);
			std::vector<CrowdTeam> t = w.c.teams;
			t[0].weapons = many;
			CHECK(!CrowdEvaluator::make(w.c.set, t, 2, config).ok());
			std::array<CrowdWeapon, 1> bad{ w.weapons[0] };
			t[0].weapons = bad;
			bad[0].binds = nullptr;
			CHECK(!CrowdEvaluator::make(w.c.set, t, 2, config).ok());
			bad[0] = w.weapons[0];
			bad[0].muzzle_joint = 99;
			CHECK(!CrowdEvaluator::make(w.c.set, t, 2, config).ok());
			bad[0] = w.weapons[0];
			bad[0].muzzle_offset.t.x = std::numeric_limits<float>::quiet_NaN();
			CHECK(!CrowdEvaluator::make(w.c.set, t, 2, config).ok());
			bad[0] = w.weapons[0];
			CrowdClipData short_reload = w.reload_w1;
			short_reload.mask.weight = { 1 };
			bad[0].reload = &short_reload;
			CHECK(!CrowdEvaluator::make(w.c.set, t, 2, config).ok());
			bad[0] = w.weapons[0];
			BoneMask short_mask;
			short_mask.weight = { 1 };
			bad[0].weapon_joints = &short_mask;
			CHECK(!CrowdEvaluator::make(w.c.set, t, 2, config).ok());
			bad[0] = w.weapons[0];
			CHECK(CrowdEvaluator::make(w.c.set, t, 2, config).ok());
		}
	}

	TEST_CASE("M2a tp stance set: stand bit for bit, stances, transitions, moves, fire, death, hostile values") {
		SynthWeapons w;
		SynthStance st(w.c);
		CrowdConfig config;
		config.temporal_lod = false;
		const double dt = 1.0 / 60.0;
		// Stand with the stance set equals S3 bit for bit over the stage schedule.
		{
			const CrowdStageSpec spec;
			auto s3 = CrowdEvaluator::make(w.c.set, w.c.teams, spec.soldiers(), config);
			auto m = CrowdEvaluator::make(w.c.set, st.set, w.teams_one, spec.soldiers(), config);
			REQUIRE(s3.ok());
			REQUIRE_MESSAGE(m.ok(), (m.ok() ? "" : m.error().message));
			double t = 0.0;
			for (int frame = 0; frame < 240; ++frame) {
				const std::vector<CrowdSoldierInput> in = stage_inputs(spec, t + dt, t, false);
				t += dt;
				(*s3)->set_inputs(in, dt);
				(*s3)->evaluate_all_here();
				(*m)->set_inputs(in, dt);
				(*m)->evaluate_all_here();
				for (uint32_t i = 0; i < spec.soldiers(); ++i) {
					CHECK(palettes_equal((*s3)->palette(i), (*m)->palette(i)));
				}
			}
			CHECK((*m)->m2a_stats().stand == spec.soldiers());
			CHECK((*m)->m2a_stats().stance_fallbacks == 0);
		}
		auto e = CrowdEvaluator::make(w.c.set, st.set, w.teams_two, 8, config);
		REQUIRE(e.ok());
		CrowdEvaluator &ev = **e;
		std::vector<CrowdSoldierInput> in(8);
		in[1].pose = in[1].target_pose = 1; // crouch
		in[2].pose = in[2].target_pose = 2; // prone
		in[3].pose = 0; // stand -> crouch at 0.5
		in[3].target_pose = 1;
		in[3].transition = 0.5f;
		in[4].pose = 1; // crouch -> prone: no clip, the lerp of the idles
		in[4].target_pose = 2;
		in[4].transition = 0.25f;
		in[5].pose = in[5].target_pose = 1; // crouch moving forward
		in[5].velocity = Vec3{ 0, 0, 1.5f };
		in[6].pose = 9; // hostile
		in[6].target_pose = 1;
		in[6].transition = std::numeric_limits<float>::quiet_NaN();
		in[7].pose = in[7].target_pose = 2; // prone moving: the stand cycles (no prone moves in the set), counted
		in[7].velocity = Vec3{ 0, 0, 1.0f };
		ev.set_inputs(in, dt);
		ev.evaluate_all_here();
		CHECK(hips_y(ev, 0) == doctest::Approx(1.0f).epsilon(1e-4));
		CHECK(hips_y(ev, 1) == doctest::Approx(0.6f).epsilon(1e-4));
		CHECK(hips_y(ev, 2) == doctest::Approx(0.2f).epsilon(1e-4));
		CHECK(hips_y(ev, 3) == doctest::Approx(0.8f).epsilon(1e-3));
		CHECK(hips_y(ev, 4) == doctest::Approx(0.5f).epsilon(1e-3)); // 0.6 + (0.2 - 0.6) x 0.25
		CrowdM2aStats ms = ev.m2a_stats();
		CHECK(ms.transitions == 3); // 3, 4 and the hostile 6 (2 -> 1 at fraction 0)
		CHECK(ms.crouch == 2);
		CHECK(ms.prone == 2);
		CHECK(ms.stand == 1);
		CHECK(ms.moving_crouch == 1);
		CHECK(ms.moving_prone == 1);
		CHECK(ms.bad_stance == 2);
		CHECK(ms.stance_fallbacks == 3); // the crouch -> prone lerp, the hostile prone -> crouch lerp, prone moves
		for (uint32_t i = 0; i < 8; ++i) {
			for (const Mat34 &m : ev.palette(i)) {
				for (const auto &row : m.m) {
					for (float v : row) {
						CHECK(std::isfinite(v));
					}
				}
			}
		}
		// Fire: a fire_seq change kicks the stand aim for 50 ms (the Arm turns further up); crouch plays the fire clip.
		ev.set_inputs(in, dt);
		ev.evaluate_all_here();
		CHECK(ev.m2a_stats().firing == 0);
		const Quat arm_rest = ev.model(0)[2].q;
		in[0].fire_seq = 8;
		in[1].fire_seq = 1;
		ev.set_inputs(in, dt);
		ev.evaluate_all_here();
		CHECK(ev.m2a_stats().firing == 2); // 0's kick, 1's crouch fire
		const float kick_deg = quat_angle_deg(arm_rest, ev.model(0)[2].q);
		CHECK(kick_deg > 0.5f); // 30 deg x 3 / 78.1
		CHECK(kick_deg < 2.0f);
		for (int f = 0; f < 4; ++f) { // 67 ms
			ev.set_inputs(in, dt);
			ev.evaluate_all_here();
		}
		CHECK(ev.m2a_stats().firing == 1); // the kick is over, the 0.7 s crouch fire is not
		CHECK(same_quat(arm_rest, ev.model(0)[2].q));
		for (int f = 0; f < 40; ++f) { // 0.73 s in all
			ev.set_inputs(in, dt);
			ev.evaluate_all_here();
		}
		CHECK(ev.m2a_stats().firing == 0);
		// The fire_seq of a soldier's first evaluation is not a shot.
		{
			auto f = CrowdEvaluator::make(w.c.set, st.set, w.teams_one, 1, config);
			REQUIRE(f.ok());
			std::vector<CrowdSoldierInput> one(1);
			one[0].fire_seq = 5;
			for (int k = 0; k < 3; ++k) {
				(*f)->set_inputs(one, dt);
				(*f)->evaluate_all_here();
				CHECK((*f)->m2a_stats().firing == 0);
			}
		}
		// Death (Q13 default): the pose blends to the prone idle over 0.5 s, hidden after 3 s, a respawn restarts it.
		in[0].dead = true;
		ev.set_inputs(in, dt);
		ev.evaluate_all_here();
		CHECK(ev.m2a_stats().deaths == 1);
		CHECK(ev.m2a_stats().dying == 1);
		for (int f = 0; f < 15; ++f) {
			ev.set_inputs(in, dt);
			ev.evaluate_all_here();
		}
		CHECK(hips_y(ev, 0) < 0.95f);
		CHECK(hips_y(ev, 0) > 0.25f);
		for (int f = 0; f < 30; ++f) {
			ev.set_inputs(in, dt);
			ev.evaluate_all_here();
		}
		CHECK(hips_y(ev, 0) == doctest::Approx(0.2f).epsilon(1e-4));
		CHECK(ev.drawn(0));
		CHECK(ev.weapon_of(0) == 0);
		for (int f = 0; f < 150; ++f) { // to 3.0 s
			ev.set_inputs(in, dt);
			ev.evaluate_all_here();
		}
		CHECK(!ev.drawn(0));
		CHECK(ev.weapon_of(0) == kNoCrowdWeapon);
		Xform mz;
		CHECK(!ev.muzzle_model(0, mz));
		CHECK(ev.m2a_stats().death_hidden == 1);
		in[0].dead = false;
		ev.set_inputs(in, dt);
		ev.evaluate_all_here();
		CHECK(ev.drawn(0));
		CHECK(hips_y(ev, 0) == doctest::Approx(1.0f).epsilon(1e-4));
		// The clip style: the stance's death clip, its last frame held.
		{
			SynthStance clip_style(w.c, CrowdDeathStyle::Clip);
			auto d = CrowdEvaluator::make(w.c.set, clip_style.set, w.teams_one, 1, config);
			REQUIRE(d.ok());
			std::vector<CrowdSoldierInput> one(1);
			one[0].dead = true;
			for (int f = 0; f < 60; ++f) {
				(*d)->set_inputs(one, dt);
				(*d)->evaluate_all_here();
			}
			CHECK(hips_y(**d, 0) == doctest::Approx(0.1f).epsilon(1e-4));
			CHECK((*d)->drawn(0));
		}
		// A dying soldier is due every frame under temporal LOD (far away, where a live one is skipped).
		{
			CrowdConfig lod;
			lod.temporal_lod = true;
			auto d = CrowdEvaluator::make(w.c.set, st.set, w.teams_one, 2, lod);
			REQUIRE(d.ok());
			std::vector<CrowdSoldierInput> two(2);
			two[0].camera_distance = two[1].camera_distance = 180.0f;
			two[1].dead = true;
			uint32_t live = 0, dying = 0;
			for (int f = 0; f < 60; ++f) {
				(*d)->set_inputs(two, 1.0 / 144.0);
				(*d)->evaluate_all_here();
				live += (*d)->changed(0) ? 1 : 0;
				dying += (*d)->changed(1) ? 1 : 0;
			}
			CHECK(dying == 60);
			CHECK(live < 30);
		}
		// The stance clips must be on the crowd rig.
		{
			SynthStance other(w.c);
			std::vector<Bone> bones(2);
			bones[0].name = "AITrajectory";
			bones[1].name = "Other";
			bones[1].parent = 0;
			other.pose.rig = *Skeleton::make(std::move(bones));
			CHECK(!CrowdEvaluator::make(w.c.set, other.set, w.teams_one, 1, config).ok());
		}
	}
}

// ---- M2a tp: E18 (b) (docs/design/m2a.md E18, §6.5; docs/design/m2a-tp.md) -------------------------------------------
// Case "capsules": lc's capsule pose table (built from the install as anim.server_pose builds it) against CrowdEvaluator
// with tp's stance set: the crouch and prone rows at the 32 mid-pitches within 3 cm (head and chest), the six transition
// rows at the 15 mid-fractions x 32 mid-pitches reported (and checked against the reference evaluation), a moving
// crouch / prone soldier reported (locomotion is not modelled). In the install suite (anim.s3): the case needs the retail
// clips and capsules, so it cannot run in the data-free anim.s3.unit row m2a.md names (m2a-tp.md records the deviation).

#include "bf3/anim/server_pose_reference.h"
#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/sim/weapons/posed_target.h"
#include "bf3/sim/weapons/weapon_install.h"

namespace {

namespace tp_gp = bf3::core::gameplay;
namespace tp_sw = bf3::sim::weapons;
namespace tp_ref = bf3::anim::server_pose_reference;

struct TpDev {
	double head = 0, chest = 0, body = 0, legs = 0; // cm, max over the queries
	size_t queries = 0;
};

double &tp_slot(TpDev &r_dev, tp_sw::HitZone p_zone) {
	return p_zone == tp_sw::HitZone::Head ? r_dev.head : p_zone == tp_sw::HitZone::Chest ? r_dev.chest : p_zone == tp_sw::HitZone::Legs ? r_dev.legs : r_dev.body;
}

void tp_accumulate(TpDev &r_dev, const tp_sw::HitTarget &p_posed, const std::vector<tp_ref::RefCapsule> &p_ref) {
	REQUIRE(p_posed.capsule_count == p_ref.size());
	for (size_t c = 0; c < p_ref.size(); ++c) {
		const tp_sw::ZoneCapsule &z = p_posed.capsules[c];
		double d2 = 0;
		for (size_t k = 0; k < 3; ++k) {
			const double pc = 0.5 * (double(z.a[k]) + double(z.b[k]));
			d2 += (pc - p_ref[c].centre[k]) * (pc - p_ref[c].centre[k]);
		}
		double &slot = tp_slot(r_dev, z.zone);
		slot = std::max(slot, std::sqrt(d2) * 100.0);
	}
	++r_dev.queries;
}

// Two placements of the same capsules (the evaluator's against the reference's); the zones from p_zones.
void tp_accumulate_ref(TpDev &r_dev, const std::vector<tp_ref::RefCapsule> &p_a, const std::vector<tp_ref::RefCapsule> &p_b, const tp_sw::HitTarget &p_zones) {
	REQUIRE(p_a.size() == p_b.size());
	REQUIRE(p_zones.capsule_count == p_a.size());
	for (size_t c = 0; c < p_a.size(); ++c) {
		double d2 = 0;
		for (size_t k = 0; k < 3; ++k) {
			d2 += (p_a[c].centre[k] - p_b[c].centre[k]) * (p_a[c].centre[k] - p_b[c].centre[k]);
		}
		double &slot = tp_slot(r_dev, p_zones.capsules[c].zone);
		slot = std::max(slot, std::sqrt(d2) * 100.0);
	}
	++r_dev.queries;
}

tp_sw::HitTarget tp_posed(const tp_gp::CapsulePoseTable &p_table, uint8_t p_from, uint8_t p_to, double p_fraction, double p_pitch_deg) {
	tp_sw::TargetPose q;
	q.pose = bf3::sim::Pose(p_from);
	q.target_pose = bf3::sim::Pose(p_to);
	q.transition = float(p_fraction);
	q.pitch = float(p_pitch_deg * double(kPi) / 180.0);
	return tp_sw::posed_target(p_table, q);
}

} // namespace

TEST_SUITE("anim-s3") {
	TEST_CASE("capsules") {
		S3Data *d = s3_or_skip();
		if (d == nullptr) {
			return;
		}
		static const char *kStanceName[3] = { "stand", "crouch", "prone" };
		static const char *kTransitionName[6] = { "stand->crouch", "stand->prone", "crouch->stand", "crouch->prone", "prone->stand", "prone->crouch" };
		static constexpr uint8_t kTrFrom[6] = { 0, 0, 1, 1, 2, 2 };
		static constexpr uint8_t kTrTo[6] = { 1, 2, 0, 2, 0, 1 };
		constexpr double kGateCm = 3.0; // E18 (b) [inferred, m2a.md §16 Q14]
		constexpr double kTransitionGateCm = 5.0; // E18's transition rows (lc's table-vs-reference gate; reported here)
		// lc's table, as anim.server_pose builds it.
		auto weapons = tp_sw::open_install_weapons(d->install);
		REQUIRE_MESSAGE(weapons.ok(), (weapons.ok() ? "" : weapons.error().message));
		auto cd = tp_sw::capsule_table_data(weapons->soldier, weapons->capsules, *weapons->materials);
		REQUIRE_MESSAGE(cd.ok(), (cd.ok() ? "" : cd.error().message));
		auto pose = load_server_pose_clips(*d->source, *d->library);
		REQUIRE_MESSAGE(pose.ok(), (pose.ok() ? "" : pose.error().message));
		CapsuleTableInputs inputs;
		inputs.capsules = weapons->capsules;
		inputs.property = cd->property;
		inputs.zone = cd->zone;
		inputs.standin = cd->standin;
		CapsuleTableReport report;
		auto table = build_capsule_pose_table(pose->rig, *pose, inputs, &report);
		REQUIRE_MESSAGE(table.ok(), (table.ok() ? "" : table.error().message));
		const tp_gp::CapsulePoseTable &t = *table;
		auto r = tp_ref::Reference::make(*pose, weapons->capsules, report.aim, CrowdConfig{}.aim_order);
		REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
		// tp's moves and the evaluator with the stance set (one bind: the capsules come from the model pose).
		auto moves = load_crowd_move_clips(d->clips, *d->library);
		REQUIRE_MESSAGE(moves.ok(), (moves.ok() ? "" : moves.error().message));
		for (const std::string &n : moves->notes) {
			MESSAGE("move clips: " << n);
		}
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
		REQUIRE(binds.bind(ebx, 0, d->clips.rig).ok());
		CrowdTeam team;
		team.binds = &binds;
		CrowdStanceSet set;
		set.pose = &*pose;
		set.moves = &*moves;
		CrowdConfig config;
		config.temporal_lod = false;
		auto ev = CrowdEvaluator::make(d->clips, set, std::span<const CrowdTeam>(&team, 1), kMaxCrowdSoldiers, config);
		REQUIRE_MESSAGE(ev.ok(), (ev.ok() ? "" : ev.error().message));
		std::vector<double> pitches;
		for (size_t i = 0; i + 1 < t.pitch_deg.size(); ++i) {
			pitches.push_back(0.5 * (double(t.pitch_deg[i]) + double(t.pitch_deg[i + 1])));
		}
		REQUIRE(pitches.size() == 32);
		// Query batches of up to kMaxCrowdSoldiers soldiers, two frames each (the steady poses do not depend on time).
		struct Q {
			uint8_t from, to;
			double fraction, pitch;
		};
		uint64_t fallbacks = 0;
		auto run = [&](const std::vector<Q> &p_q, auto &&p_each) {
			for (size_t base = 0; base < p_q.size(); base += kMaxCrowdSoldiers) {
				const size_t n = std::min<size_t>(kMaxCrowdSoldiers, p_q.size() - base);
				std::vector<CrowdSoldierInput> in(kMaxCrowdSoldiers);
				for (size_t i = 0; i < kMaxCrowdSoldiers; ++i) {
					in[i].flags = crowd_flags::kHidden;
				}
				for (size_t i = 0; i < n; ++i) {
					const Q &q = p_q[base + i];
					in[i].flags = 0;
					in[i].pose = q.from;
					in[i].target_pose = q.to;
					in[i].transition = float(q.fraction);
					in[i].pitch = float(q.pitch * double(kPi) / 180.0);
				}
				for (int f = 0; f < 2; ++f) {
					(*ev)->set_inputs(in, 1.0 / 60.0);
					(*ev)->evaluate_all_here();
				}
				for (size_t i = 0; i < n; ++i) {
					p_each(p_q[base + i], (*ev)->model(uint32_t(i)));
				}
			}
			fallbacks = (*ev)->m2a_stats().stance_fallbacks;
		};
		std::vector<tp_ref::RefCapsule> rc;
		// The steady rows: stand (E18 (a) again, now through the stance path), crouch and prone (E18 (b), the gate).
		for (uint8_t s = 0; s < 3; ++s) {
			std::vector<Q> qs;
			for (const double p : pitches) {
				qs.push_back(Q{ s, s, 0.0, p });
			}
			TpDev dev;
			run(qs, [&](const Q &p_q, std::span<const Xform> p_model) {
				r->place(p_model, rc);
				tp_accumulate(dev, tp_posed(t, p_q.from, p_q.to, 0.0, p_q.pitch), rc);
			});
			std::printf("anim.s3 E18 (b) capsules %-6s (CrowdEvaluator, stance set) %zu queries: head %.3f cm chest %.3f cm (gate %.0f cm) body %.3f cm legs %.3f cm "
						"(reported)\n",
					kStanceName[s], dev.queries, dev.head, dev.chest, kGateCm, dev.body, dev.legs);
			CHECK(dev.queries == 32);
			CHECK_MESSAGE(dev.head <= kGateCm, std::string(kStanceName[s]) << " head " << dev.head << " cm");
			CHECK_MESSAGE(dev.chest <= kGateCm, std::string(kStanceName[s]) << " chest " << dev.chest << " cm");
		}
		CHECK(fallbacks == 0);
		// The transition rows: against the table (reported; E18's 5 cm is lc's table-vs-reference gate) and against the
		// reference evaluation (the evaluator samples the clip at the exact fraction as the reference does).
		for (uint8_t tr = 0; tr < 6; ++tr) {
			std::vector<Q> qs;
			for (size_t f = 0; f + 1 < tp_gp::kCapsuleTransitionFractions; ++f) {
				const double frac = (double(f) + 0.5) / double(tp_gp::kCapsuleTransitionFractions - 1);
				for (const double p : pitches) {
					qs.push_back(Q{ kTrFrom[tr], kTrTo[tr], frac, p });
				}
			}
			TpDev dev, vs_ref;
			std::vector<tp_ref::RefCapsule> want;
			run(qs, [&](const Q &p_q, std::span<const Xform> p_model) {
				r->place(p_model, rc);
				const tp_sw::HitTarget posed = tp_posed(t, p_q.from, p_q.to, p_q.fraction, p_q.pitch);
				tp_accumulate(dev, posed, rc);
				tp_ref::Query q;
				q.transition = true;
				q.row = tr;
				q.pitch_deg = p_q.pitch;
				q.fraction = p_q.fraction;
				REQUIRE(r->evaluate(q, want));
				tp_accumulate_ref(vs_ref, rc, want, posed);
			});
			std::printf("anim.s3 E18 (b) capsules transition %-14s %zu queries vs the table: head %.3f cm chest %.3f cm (lc's gate %.0f cm, reported) body %.3f "
						"legs %.3f cm; vs the reference: head %.3f chest %.3f body %.3f legs %.3f cm\n",
					kTransitionName[tr], dev.queries, dev.head, dev.chest, kTransitionGateCm, dev.body, dev.legs, vs_ref.head, vs_ref.chest, vs_ref.body,
					vs_ref.legs);
			CHECK(dev.queries == (tp_gp::kCapsuleTransitionFractions - 1) * 32);
			CHECK(vs_ref.head <= kGateCm);
			CHECK(vs_ref.chest <= kGateCm);
		}
		CHECK(fallbacks == 0);
		// A moving crouch / prone soldier against its steady row (locomotion is not modelled; reported).
		for (uint8_t s = 1; s < 3; ++s) {
			std::vector<CrowdSoldierInput> in(1);
			in[0].pose = in[0].target_pose = s;
			in[0].velocity = Vec3{ 0, 0, s == 1 ? 2.0f : 0.8f };
			auto mv = CrowdEvaluator::make(d->clips, set, std::span<const CrowdTeam>(&team, 1), 1, config);
			REQUIRE(mv.ok());
			for (int f = 0; f < 40; ++f) {
				(*mv)->set_inputs(in, 1.0 / 60.0);
				(*mv)->evaluate_all_here();
			}
			TpDev dev;
			r->place((*mv)->model(0), rc);
			tp_accumulate(dev, tp_posed(t, s, s, 0.0, 0.0), rc);
			const CrowdM2aStats ms = (*mv)->m2a_stats();
			const bool moving = (s == 1 ? ms.moving_crouch : ms.moving_prone) == 1;
			std::printf("anim.s3 E18 (b) capsules %s moving %.1f m/s vs the steady row (reported): head %.3f cm chest %.3f cm body %.3f cm legs %.3f cm; cycles: %s\n",
					kStanceName[s], double(in[0].velocity.z), dev.head, dev.chest, dev.body, dev.legs,
					!moving ? "none" : (moves->moves_ok[s - 1] ? "the stance's own" : "the stand cycles"));
			CHECK(moving);
		}
		const bf3::core::SnapshotDiff diff = bf3::core::diff_snapshots(d->before, d->reader.snapshot());
		CHECK(diff.clean());
	}
}

// ---- M2b cam (docs/design/m2b.md §4, §2.6 #17; as built: docs/design/m2b-cam.md) -------------------------------------
// anim-s3-unit (synthetic, no data): the "revive" case. CrowdStanceSet::corpse_seconds keeps a corpse drawn for 15 s
// instead of M2a's 3 s; a dead soldier that comes back with CrowdSoldierInput::revived stands up from its corpse pose
// over kCrowdReviveBlendSeconds (crowd_eval.cpp [revive]), while a plain respawn is standing at once; the stand-up is
// due every frame under temporal LOD; make() refuses a hostile corpse time.

TEST_SUITE("anim-s3-unit") {
	TEST_CASE("revive") {
		SynthWeapons w;
		SynthStance st(w.c);
		st.set.corpse_seconds = kCrowdCorpseSeconds;
		CrowdConfig config;
		config.temporal_lod = false;
		const double dt = 1.0 / 60.0;
		auto e = CrowdEvaluator::make(w.c.set, st.set, w.teams_one, 3, config);
		REQUIRE_MESSAGE(e.ok(), (e.ok() ? "" : e.error().message));
		CrowdEvaluator &ev = **e;
		auto run = [&](std::vector<CrowdSoldierInput> &p_in, int p_frames) {
			for (int f = 0; f < p_frames; ++f) {
				ev.set_inputs(p_in, dt);
				ev.evaluate_all_here();
			}
		};
		// 0 is revived, 1 respawns, 2 stays dead.
		std::vector<CrowdSoldierInput> in(3);
		run(in, 2);
		for (CrowdSoldierInput &i : in) {
			i.dead = true;
		}
		run(in, 120); // 2 s: prone (the Q13 blend is over)
		for (uint32_t i = 0; i < 3; ++i) {
			CHECK(hips_y(ev, i) == doctest::Approx(0.2f).epsilon(1e-4));
		}
		run(in, 180); // 5 s: M2a would have hidden them at 3 s
		for (uint32_t i = 0; i < 3; ++i) {
			CHECK(ev.drawn(i));
		}
		CHECK(ev.m2a_stats().death_hidden == 0);
		CHECK(ev.m2a_stats().dying == 3);
		// The revive and the respawn.
		in[0].dead = false;
		in[0].revived = true;
		in[1].dead = false;
		run(in, 1);
		CHECK(ev.m2a_stats().revives == 1);
		CHECK(hips_y(ev, 0) == doctest::Approx(0.2f).epsilon(1e-4)); // the stand-up starts at the corpse pose
		CHECK(hips_y(ev, 1) == doctest::Approx(1.0f).epsilon(1e-4)); // a respawn stands at once
		CHECK(ev.drawn(0));
		run(in, 15); // half of the 0.5 s
		CHECK(hips_y(ev, 0) == doctest::Approx(0.6f).epsilon(1e-3));
		CHECK(ev.m2a_stats().dying == 1); // only 2 is still a corpse
		run(in, 16);
		CHECK(hips_y(ev, 0) == doctest::Approx(1.0f).epsilon(1e-4)); // standing: the stand-up is over
		run(in, 30);
		CHECK(ev.m2a_stats().revives == 1); // revived stays set over the life: no second stand-up without a death
		// 2 is hidden at 15 s after its death (about 5.8 s so far).
		run(in, 538); // 899 frames dead: just below 15 s
		CHECK(ev.drawn(2));
		run(in, 3);
		CHECK(!ev.drawn(2));
		CHECK(ev.m2a_stats().death_hidden == 1);
		// A revived life that dies again falls again, and a second revive stands it up again.
		in[0].dead = true;
		run(in, 60);
		CHECK(hips_y(ev, 0) == doctest::Approx(0.2f).epsilon(1e-4));
		in[0].dead = false;
		run(in, 1);
		CHECK(ev.m2a_stats().revives == 2);
		CHECK(hips_y(ev, 0) < 0.25f);
		// A stand-up is due every frame under temporal LOD (far away, where a living soldier is skipped).
		{
			CrowdConfig lod;
			lod.temporal_lod = true;
			auto d = CrowdEvaluator::make(w.c.set, st.set, w.teams_one, 2, lod);
			REQUIRE(d.ok());
			std::vector<CrowdSoldierInput> two(2);
			two[0].camera_distance = two[1].camera_distance = 180.0f;
			two[0].dead = true;
			for (int f = 0; f < 144; ++f) {
				(*d)->set_inputs(two, 1.0 / 144.0);
				(*d)->evaluate_all_here();
			}
			two[0].dead = false;
			two[0].revived = true;
			uint32_t live = 0, rising = 0;
			for (int f = 0; f < 60; ++f) { // 0.42 s of the 0.5 s stand-up
				(*d)->set_inputs(two, 1.0 / 144.0);
				(*d)->evaluate_all_here();
				live += (*d)->changed(1) ? 1 : 0;
				rising += (*d)->changed(0) ? 1 : 0;
			}
			CHECK(rising == 60);
			CHECK(live < 30);
			CHECK((*d)->m2a_stats().revives == 1);
		}
		// M2a's default corpse time is unchanged (3 s), and make() refuses a hostile one.
		{
			SynthStance m2a(w.c);
			CHECK(m2a.set.corpse_seconds == kCrowdDeathHideSeconds);
			for (const float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f, kCrowdMaxCorpseSeconds + 1.0f }) {
				SynthStance h(w.c);
				h.set.corpse_seconds = bad;
				CHECK(!CrowdEvaluator::make(w.c.set, h.set, w.teams_one, 1, config).ok());
			}
		}
	}
}
