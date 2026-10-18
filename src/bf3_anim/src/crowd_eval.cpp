// Pose evaluation of the 3p crowd (S3; docs/design/s3.md §3, §5). See crowd_eval.h.
#include "bf3/anim/crowd_eval.h"

#include "bf3/core/worker_group.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <string>
#include <tuple>

namespace bf3::anim {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kReloadRampSeconds = 0.15f; // s3.md §5.4
constexpr float kDegToRad = 0.0174532925f;

bool finite3(const Vec3 &p_v) {
	return std::isfinite(p_v.x) && std::isfinite(p_v.y) && std::isfinite(p_v.z);
}

// Stagger offset of slot p_i in [0, 1) (golden-ratio sequence: neighbouring slots spread evenly).
float stagger_of(uint32_t p_i) {
	const double v = double(p_i) * 0.6180339887498949;
	return float(v - std::floor(v));
}

} // namespace

struct CrowdEvaluator::Impl {
	struct Team {
		const SkinBinds *binds = nullptr;
		const CrowdClipData *reload = nullptr;
		float reload_rate = 1.0f;
		float reload_seconds = 0.0f; // at the rate
		const BoneMask *weapon_joints = nullptr;
		std::vector<Xform> weapon_local;
	};

	// Everything one soldier owns. Written by its lane between kick and wait, by the owner thread otherwise.
	struct Soldier {
		CrowdSoldierInput input;
		bool reload_pending = false; // kReloadStart seen by set_inputs, consumed by the next evaluation
		float reload_t = -1.0f; // seconds into the reload (< 0: none)
		float phase = 0.0f; // shared normalized locomotion phase
		double clock = 0.0; // seconds of input time seen
		double last_eval_clock = 0.0;
		int64_t lod_bucket = INT64_MIN;
		bool ever = false; // evaluated at least once
		// Per-soldier double buffers.
		std::array<std::vector<Mat34>, 2> palette;
		std::array<std::vector<Xform>, 2> model;
		std::array<uint32_t, 2> binds{}; // palette size of each slot (the team's binds at evaluation)
		uint8_t front = 0;
		bool evaluated = false; // by the current job
		bool changed = false; // in the last publish
		// Scratch (lane only).
		std::array<LocalPose, 3> sample; // up to two cycles and the layer sample
		LocalPose pose;
		core::ant::ClipPose scratch;
		// The job's per-soldier counters (merged at publish).
		bool skipped_lod = false;
		bool hidden = false;
		uint8_t loco_clips = 0;
		bool two_cycle = false, idle_mix = false, reloading = false, aiming = false;
		int8_t tier = -1;
		int64_t done_ns = 0;
	};

	const CrowdClipSet *clips = nullptr;
	CrowdConfig config;
	std::vector<Team> teams;
	std::vector<Soldier> soldiers;
	LocalPose idle; // Rifle_StandIdle_Pose sampled once (a pose)
	LocalPose aim_up, aim_down; // the additive deltas sampled once onto identity (end frame)
	const BoneMask *aim_up_mask = nullptr;
	const BoneMask *aim_down_mask = nullptr;
	double dt = 0.0;
	uint64_t bad_inputs = 0;
	CrowdFrameStats stats;
	bool job = false; // kicked and not yet published
	Clock::time_point kick_time{};

	void evaluate(uint32_t p_i);
	void publish(double p_blocked);
	void prepare_job();
	// M2a tp [impl] (named insertion, docs/design/m2a-tp.md): R5's per-team weapons and the stance set, per team and per
	// soldier (the M2a tp block at the end of this file). Null: S3's evaluator exactly (no stance set, no team with weapons).
	struct M2a;
	struct M2aDeleter {
		void operator()(M2a *p_m2a) const;
	};
	std::unique_ptr<M2a, M2aDeleter> m2a;
	void evaluate_m2a(uint32_t p_i);
};

void CrowdEvaluator::Impl::prepare_job() {
	for (Soldier &s : soldiers) {
		s.evaluated = false;
	}
	kick_time = Clock::now();
	job = true;
}

void CrowdEvaluator::Impl::evaluate(uint32_t p_i) {
	if (m2a) { // M2a tp [eval] (named insertion): the stance and per-soldier weapon path (the M2a tp block)
		evaluate_m2a(p_i);
		return;
	}
	Soldier &s = soldiers[p_i];
	s.skipped_lod = s.hidden = s.two_cycle = s.idle_mix = s.reloading = s.aiming = false;
	s.loco_clips = 0;
	s.tier = int8_t(temporal_lod_tier(config.lod, s.input.camera_distance));
	s.clock += dt;
	const auto finish = [&] { s.done_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); };
	if ((s.input.flags & crowd_flags::kHidden) != 0) {
		s.hidden = true;
		finish();
		return;
	}
	// Temporal LOD: due when the soldier's staggered bucket of its tier interval changes.
	if (config.temporal_lod && s.ever) {
		const float interval = temporal_lod_interval(config.lod, s.input.camera_distance);
		bool due = true;
		if (interval < 0.0f) {
			due = false; // FarPolicy::Freeze
		} else if (interval > 0.0f) {
			const int64_t bucket = int64_t(std::floor(s.clock / double(interval) + double(stagger_of(p_i))));
			due = bucket != s.lod_bucket;
			s.lod_bucket = bucket;
		}
		if (!due) {
			s.skipped_lod = true;
			finish();
			return;
		}
	} else if (config.temporal_lod) {
		const float interval = temporal_lod_interval(config.lod, s.input.camera_distance);
		if (interval > 0.0f) {
			s.lod_bucket = int64_t(std::floor(s.clock / double(interval) + double(stagger_of(p_i))));
		}
	}
	const float step = float(std::max(0.0, s.clock - s.last_eval_clock));
	s.last_eval_clock = s.clock;
	s.ever = true;
	const uint32_t team_index = std::min<uint32_t>(s.input.team, uint32_t(teams.size() - 1));
	const Team &team = teams[team_index];
	const Skeleton &rig = clips->rig;

	// 1. Run blend at one phase.
	const float yaw = s.input.yaw;
	const float cy = std::cos(yaw), sy = std::sin(yaw);
	const Vec3 v = s.input.velocity;
	const Vec3 local{ v.x * cy - v.z * sy, 0.0f, v.x * sy + v.z * cy };
	const LocoWeights w = locomotion_weights(clips->loco, local, config.loco);
	if (w.cycle_seconds > 0.0f) {
		s.phase = advance_phase(s.phase, step, w.cycle_seconds);
	}
	static constexpr std::array<CrowdClip, kLocoClips> kClipOf{ CrowdClip::Idle, CrowdClip::RunFwd, CrowdClip::RunBwd, CrowdClip::RunLeft, CrowdClip::RunRight };
	std::array<const LocalPose *, 3> inputs{};
	std::array<float, 3> weights{};
	size_t n = 0;
	uint32_t cycles = 0;
	for (size_t c = 0; c < kLocoClips && n < 3; ++c) {
		if (!(w.weight[c] > 0.0f)) {
			continue;
		}
		if (c == size_t(LocoClip::Idle)) {
			inputs[n] = &idle;
		} else {
			const CrowdClipData &clip = clips->get(kClipOf[c]);
			LocalPose &dst = s.sample[cycles];
			sample_pose(rig, clip.clip, clip.tracks, s.phase * float(clip.tracks.end_frame), dst, s.scratch);
			inputs[n] = &dst;
			++cycles;
		}
		weights[n] = w.weight[c];
		++n;
	}
	s.loco_clips = uint8_t(n);
	s.two_cycle = cycles >= 2;
	s.idle_mix = w.weight[size_t(LocoClip::Idle)] > 0.0f && cycles >= 1;
	if (n == 1) {
		s.pose = *inputs[0];
	} else {
		blend_poses(inputs.data(), weights.data(), n, s.pose);
	}
	// 2. The sim owns the root: its translation back to rest.
	if (clips->root_joint >= 0 && size_t(clips->root_joint) < s.pose.joints.size()) {
		s.pose.joints[size_t(clips->root_joint)].t = rig.bone(size_t(clips->root_joint)).rest.t;
	}
	// 3. Weapon layout of the joints the locomotion clips leave undriven.
	if (team.weapon_joints != nullptr && !team.weapon_local.empty()) {
		const size_t m = std::min(s.pose.joints.size(), team.weapon_joints->size());
		for (size_t j = 0; j < m; ++j) {
			if (team.weapon_joints->weight[j] > 0.0f) {
				s.pose.joints[j] = team.weapon_local[j];
			}
		}
	}
	// 4. Masked upper-body reload.
	if (s.reload_pending) {
		s.reload_pending = false;
		if (s.reload_t < 0.0f && team.reload != nullptr) {
			s.reload_t = 0.0f;
		}
	} else if (s.reload_t >= 0.0f) {
		s.reload_t += step;
	}
	if (s.reload_t >= 0.0f && team.reload != nullptr) {
		if (s.reload_t >= team.reload_seconds) {
			s.reload_t = -1.0f;
		} else {
			const float rw = std::min({ 1.0f, s.reload_t / kReloadRampSeconds, (team.reload_seconds - s.reload_t) / kReloadRampSeconds });
			const CrowdClipData &clip = *team.reload;
			LocalPose &dst = s.sample[2];
			sample_pose(rig, clip.clip, clip.tracks, s.reload_t * team.reload_rate * clip.tracks.fps, dst, s.scratch);
			override_masked(s.pose, dst, clip.mask, rw);
			s.reloading = rw > 0.0f;
		}
	}
	// 5. Additive aim.
	const float pitch = s.input.pitch;
	const bool up = pitch > 0.0f;
	const float full_deg = up ? config.aim_up_full_pitch_deg : config.aim_down_full_pitch_deg;
	if (pitch != 0.0f && full_deg > 0.0f) {
		const float aw = std::min(1.0f, std::fabs(pitch) / (full_deg * kDegToRad));
		const BoneMask *mask = up ? aim_up_mask : aim_down_mask;
		if (mask != nullptr && aw > 0.0f) {
			apply_additive(s.pose, up ? aim_up : aim_down, *mask, aw, config.aim_order);
			s.aiming = true;
		}
	}
	// 6. Model pose and skin palette into the back buffers.
	const uint8_t back = uint8_t(s.front ^ 1);
	model_pose(rig, s.pose, s.model[back]);
	skin_matrices(s.model[back], team.binds->binds(), s.palette[back]);
	s.binds[back] = uint32_t(team.binds->size());
	s.evaluated = true;
	finish();
}

void CrowdEvaluator::Impl::publish(double p_blocked) {
	if (!job) {
		return;
	}
	job = false;
	stats = CrowdFrameStats{};
	stats.blocked_seconds = p_blocked;
	int64_t last_ns = 0;
	for (Soldier &s : soldiers) {
		s.changed = s.evaluated;
		if (s.evaluated) {
			s.front ^= 1;
			++stats.evaluated;
			stats.loco_clips_sampled += s.loco_clips;
			stats.two_cycle += s.two_cycle ? 1 : 0;
			stats.idle_mix += s.idle_mix ? 1 : 0;
			stats.reloading += s.reloading ? 1 : 0;
			stats.aiming += s.aiming ? 1 : 0;
			stats.evaluated_by_tier[size_t(std::clamp<int>(s.tier + 1, 0, int(stats.evaluated_by_tier.size()) - 1))]++;
		}
		stats.skipped_lod += s.skipped_lod ? 1 : 0;
		stats.hidden += s.hidden ? 1 : 0;
		last_ns = std::max(last_ns, s.done_ns);
	}
	const int64_t kick_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(kick_time.time_since_epoch()).count();
	stats.job_seconds = last_ns > kick_ns ? double(last_ns - kick_ns) * 1e-9 : 0.0;
}

CrowdEvaluator::CrowdEvaluator() :
		impl_(std::make_unique<Impl>()) {}

CrowdEvaluator::~CrowdEvaluator() = default;

core::Result<std::unique_ptr<CrowdEvaluator>> CrowdEvaluator::make(const CrowdClipSet &p_clips, std::span<const CrowdTeam> p_teams, uint32_t p_soldiers,
		const CrowdConfig &p_config) {
	if (p_soldiers > kMaxCrowdSoldiers) {
		return core::make_error("crowd: " + std::to_string(p_soldiers) + " soldiers (cap " + std::to_string(kMaxCrowdSoldiers) + ")");
	}
	if (p_teams.empty() || p_teams.size() > kMaxCrowdTeams) {
		return core::make_error("crowd: " + std::to_string(p_teams.size()) + " teams (1.." + std::to_string(kMaxCrowdTeams) + ")");
	}
	const Skeleton &rig = p_clips.rig;
	if (rig.size() == 0 || rig.size() > kMaxCrowdRigJoints) {
		return core::make_error("crowd: rig with " + std::to_string(rig.size()) + " joints");
	}
	for (const CrowdClip c : { CrowdClip::Idle, CrowdClip::RunFwd, CrowdClip::RunBwd, CrowdClip::RunLeft, CrowdClip::RunRight }) {
		if (p_clips.get(c).tracks.rotation_bone.empty() && p_clips.get(c).tracks.translation_bone.empty()) {
			return core::make_error("crowd: locomotion clip " + std::to_string(int(c)) + " has no tracks");
		}
	}
	auto e = std::unique_ptr<CrowdEvaluator>(new CrowdEvaluator());
	Impl &m = *e->impl_;
	m.clips = &p_clips;
	m.config = p_config;
	uint32_t max_binds = 0;
	for (size_t t = 0; t < p_teams.size(); ++t) {
		const CrowdTeam &in = p_teams[t];
		const std::string where = "crowd team " + std::to_string(t);
		if (in.binds == nullptr || in.binds->size() == 0 || in.binds->size() > kMaxPaletteBinds) {
			return core::make_error(where + ": " + std::to_string(in.binds == nullptr ? 0 : in.binds->size()) + " binds (1.." + std::to_string(kMaxPaletteBinds) +
					")");
		}
		for (const SkinBind &b : in.binds->binds()) {
			if (b.bone < 0 || size_t(b.bone) >= rig.size()) {
				return core::make_error(where + ": bind " + b.name + " names bone " + std::to_string(b.bone) + " outside the rig");
			}
		}
		Impl::Team team;
		team.binds = in.binds;
		team.reload = in.reload;
		team.reload_rate = std::isfinite(in.reload_rate) && in.reload_rate > 0.0f ? in.reload_rate : 1.0f;
		if (in.reload != nullptr) {
			if (in.reload->mask.size() != rig.size()) {
				return core::make_error(where + ": reload mask is not rig-sized");
			}
			team.reload_seconds = in.reload->tracks.duration_seconds() / team.reload_rate;
		}
		if (in.weapon_joints != nullptr || !in.weapon_local.empty()) {
			if (in.weapon_joints == nullptr || in.weapon_joints->size() != rig.size() || in.weapon_local.size() != rig.size()) {
				return core::make_error(where + ": weapon layout mask / poses are not rig-sized (" + std::to_string(rig.size()) + ")");
			}
			team.weapon_joints = in.weapon_joints;
			team.weapon_local.assign(in.weapon_local.begin(), in.weapon_local.end());
		}
		max_binds = std::max(max_binds, uint32_t(in.binds->size()));
		m.teams.push_back(std::move(team));
	}
	// Static poses sampled once: the idle pose, and the aim additives onto identity at their last frame.
	{
		core::ant::ClipPose scratch;
		const CrowdClipData &idle = p_clips.get(CrowdClip::Idle);
		if (!sample_pose(rig, idle.clip, idle.tracks, 0.0f, m.idle, scratch)) {
			return core::make_error("crowd: the idle clip does not sample");
		}
		for (auto [pose, mask, clip] : { std::tuple{ &m.aim_up, &m.aim_up_mask, CrowdClip::AimUp }, std::tuple{ &m.aim_down, &m.aim_down_mask, CrowdClip::AimDown } }) {
			const CrowdClipData &c = p_clips.get(clip);
			*pose = identity_pose(rig);
			if (c.mask.size() != rig.size()) {
				continue; // no aim clip: no aim layer
			}
			if (!core::ant::sample_clip(c.clip, float(c.tracks.end_frame), scratch)) {
				return core::make_error("crowd: an aim clip does not sample");
			}
			apply_clip_pose(c.tracks, scratch, *pose);
			*mask = &c.mask;
		}
	}
	m.soldiers.resize(p_soldiers);
	for (uint32_t i = 0; i < p_soldiers; ++i) {
		Impl::Soldier &s = m.soldiers[i];
		for (int k = 0; k < 2; ++k) {
			s.palette[size_t(k)].assign(max_binds, Mat34{});
			s.model[size_t(k)].assign(rig.size(), xform_identity());
		}
		for (LocalPose &p : s.sample) {
			p = rest_pose(rig);
		}
		s.pose = rest_pose(rig);
		s.binds = { uint32_t(m.teams[0].binds->size()), uint32_t(m.teams[0].binds->size()) };
	}
	return e;
}

uint32_t CrowdEvaluator::soldiers() const {
	return uint32_t(impl_->soldiers.size());
}

void CrowdEvaluator::set_inputs(std::span<const CrowdSoldierInput> p_inputs, double p_dt) {
	Impl &m = *impl_;
	if (m.job) {
		return; // owner-thread misuse: never touch soldiers while the job may run
	}
	if (std::isfinite(p_dt) && p_dt >= 0.0 && p_dt < 1.0e6) {
		m.dt = p_dt;
	} else {
		++m.bad_inputs; // keep the last finite dt
	}
	const size_t n = std::min(p_inputs.size(), m.soldiers.size());
	for (size_t i = 0; i < n; ++i) {
		Impl::Soldier &s = m.soldiers[i];
		const CrowdSoldierInput &in = p_inputs[i];
		CrowdSoldierInput next = in;
		if (!finite3(in.position)) {
			next.position = s.input.position;
			++m.bad_inputs;
		}
		if (!finite3(in.velocity)) {
			next.velocity = s.input.velocity;
			++m.bad_inputs;
		}
		if (!std::isfinite(in.yaw)) {
			next.yaw = s.input.yaw;
			++m.bad_inputs;
		}
		if (!std::isfinite(in.pitch)) {
			next.pitch = s.input.pitch;
			++m.bad_inputs;
		}
		if (!std::isfinite(in.camera_distance)) {
			next.camera_distance = s.input.camera_distance;
			++m.bad_inputs;
		}
		if ((in.flags & crowd_flags::kReloadStart) != 0) {
			s.reload_pending = true;
		}
		s.input = next;
	}
}

bool CrowdEvaluator::kick(core::WorkerGroup &p_group) {
	Impl &m = *impl_;
	if (m.job || p_group.busy()) {
		return false;
	}
	m.prepare_job();
	Impl *impl = &m;
	if (!p_group.kick(m.soldiers.size(), [impl](size_t p_i) { impl->evaluate(uint32_t(p_i)); })) {
		m.job = false;
		return false;
	}
	return true;
}

void CrowdEvaluator::wait_and_publish(core::WorkerGroup &p_group) {
	if (!impl_->job) {
		return;
	}
	const double blocked = p_group.wait();
	impl_->publish(blocked);
}

void CrowdEvaluator::evaluate_all_here() {
	Impl &m = *impl_;
	if (m.job) {
		return;
	}
	m.prepare_job();
	for (uint32_t i = 0; i < uint32_t(m.soldiers.size()); ++i) {
		m.evaluate(i);
	}
	m.publish(0.0);
}

std::span<const Mat34> CrowdEvaluator::palette(uint32_t p_i) const {
	if (p_i >= impl_->soldiers.size()) {
		return {};
	}
	const Impl::Soldier &s = impl_->soldiers[p_i];
	const std::vector<Mat34> &p = s.palette[s.front];
	return std::span<const Mat34>(p.data(), std::min<size_t>(p.size(), s.binds[s.front]));
}

bool CrowdEvaluator::changed(uint32_t p_i) const {
	return p_i < impl_->soldiers.size() && impl_->soldiers[p_i].changed;
}

std::span<const Xform> CrowdEvaluator::model(uint32_t p_i) const {
	if (p_i >= impl_->soldiers.size()) {
		return {};
	}
	const Impl::Soldier &s = impl_->soldiers[p_i];
	return s.model[s.front];
}

const CrowdFrameStats &CrowdEvaluator::stats() const {
	return impl_->stats;
}

uint64_t CrowdEvaluator::bad_inputs() const {
	return impl_->bad_inputs;
}

} // namespace bf3::anim
