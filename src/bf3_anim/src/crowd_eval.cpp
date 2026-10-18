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
	// R5 (tp, docs/design/m2a.md §15; docs/design/m2a-tp.md): S3's checks, CrowdTeam::weapons and the buffer sizes moved
	// into the stance overload (the M2a tp block); with no stance set and no team with weapons it builds S3's evaluator.
	return make(p_clips, CrowdStanceSet{}, p_teams, p_soldiers, p_config);
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

// ---- M2a tp (docs/design/m2a.md §9, R5, E18 (b); docs/design/m2a-tp.md) ----------------------------------------------
// R5's make() with CrowdTeam::weapons and the stance set, the M2a per-soldier path (crowd_eval.h's M2a tp block lists its
// steps) and the accessors W3.0 declared for tp: muzzle_model, weapon_of, weapon_palette; plus drawn and m2a_stats.

#include "bf3/anim/server_pose.h"

namespace bf3::anim {

namespace {

// lc's transition rows in core::gameplay::CapsuleTransition order (StandToCrouch .. ProneToCrouch), -1 for from == to.
int transition_row(uint8_t p_from, uint8_t p_to) {
	static constexpr int kRow[3][3] = { { -1, 0, 1 }, { 2, -1, 3 }, { 4, 5, -1 } };
	return p_from < 3 && p_to < 3 ? kRow[p_from][p_to] : -1;
}

bool same_rig(const Skeleton &p_a, const Skeleton &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (size_t j = 0; j < p_a.size(); ++j) {
		if (p_a.bone(j).name != p_b.bone(j).name || p_a.bone(j).parent != p_b.bone(j).parent) {
			return false;
		}
	}
	return true;
}

// An additive clip's delta at its last frame onto identity (S3's rule for the aim clips).
bool additive_delta(const Skeleton &p_rig, const CrowdClipData &p_clip, LocalPose &r_delta, core::ant::ClipPose &r_scratch) {
	r_delta = identity_pose(p_rig);
	if (p_clip.mask.size() != p_rig.size()) {
		return false;
	}
	if (!core::ant::sample_clip(p_clip.clip, float(p_clip.tracks.end_frame), r_scratch)) {
		return false;
	}
	apply_clip_pose(p_clip.tracks, r_scratch, r_delta);
	return true;
}

bool finite_xform(const Xform &p_x) {
	return std::isfinite(p_x.q.x) && std::isfinite(p_x.q.y) && std::isfinite(p_x.q.z) && std::isfinite(p_x.q.w) && finite3(p_x.t);
}

} // namespace

struct CrowdEvaluator::Impl::M2a {
	struct Weapon {
		const SkinBinds *binds = nullptr;
		const CrowdClipData *reload = nullptr;
		float reload_rate = 1.0f;
		float reload_seconds = 0.0f;
		const BoneMask *weapon_joints = nullptr;
		std::vector<Xform> weapon_local;
		int32_t muzzle_joint = -1;
		Xform muzzle_offset = xform_identity();
	};
	struct Team {
		std::vector<Weapon> weapons; // empty: the team's own fields are the weapon (S3), no weapon palette
	};
	// Per sim stance (0 stand, 1 crouch, 2 prone).
	struct Stance {
		const LocalPose *idle = nullptr; // stand: Impl::idle
		LocalPose idle_own;
		bool idle_fallback = false; // the stance idle is missing: the stand idle
		const LocalPose *aim_up = nullptr, *aim_down = nullptr;
		LocalPose aim_up_own, aim_down_own;
		const BoneMask *up_mask = nullptr, *down_mask = nullptr;
		float up_full_deg = 0.0f, down_full_deg = 0.0f;
		std::array<LocoClipInfo, kLocoClips> loco{};
		std::array<const CrowdClipData *, kLocoClips> cycle{}; // per LocoClip (Idle null)
		const CrowdClipData *cycle_bwd2 = nullptr; // crouch: SneakBwdRight45, blended 50/50 with cycle[Bwd]
		bool moves_fallback = false; // the stand cycles move this stance
		const CrowdClipData *death = nullptr;
	};
	struct Soldier {
		std::array<std::vector<Mat34>, 2> weapon_palette;
		std::array<uint32_t, 2> weapon_binds{};
		std::array<uint8_t, 2> weapon{ kNoCrowdWeapon, kNoCrowdWeapon };
		std::array<Xform, 2> muzzle{ xform_identity(), xform_identity() };
		std::array<bool, 2> muzzle_ok{};
		uint8_t held = kNoCrowdWeapon; // the weapon of the last evaluation (a change cancels the reload)
		uint8_t fire_seq = 0;
		bool fire_seen = false;
		double fire_since = -1.0e9;
		bool dead = false;
		double dead_since = 0.0;
		uint8_t death_stance = 0;
		bool hidden = false; // hidden after the death timer
		bool hide_counted = false;
		LocalPose extra, extra2, alive; // lane scratch
		// The job's flags (read by m2a_stats after publish).
		uint8_t stance = 0;
		bool transition = false, moving = false, firing = false, dying = false, wrote_weapon = false, wrote_muzzle = false;
		// Since make().
		uint64_t fallbacks = 0, bad_weapon = 0, bad_stance = 0, cancelled = 0, deaths = 0, hidden_count = 0;
	};

	std::vector<Team> teams;
	std::array<Stance, 3> stances;
	bool stance_set = false; // a ServerPoseClips was given
	std::array<const CrowdClipData *, core::gameplay::kCapsuleTransitions> transitions{};
	const CrowdClipData *crouch_fire = nullptr;
	CrowdDeathStyle death = kCrowdDeathDefault;
	LocalPose identity; // the additive base of the crouch fire
	std::vector<Soldier> soldiers;
};

void CrowdEvaluator::Impl::M2aDeleter::operator()(M2a *p_m2a) const {
	delete p_m2a;
}

core::Result<std::unique_ptr<CrowdEvaluator>> CrowdEvaluator::make(const CrowdClipSet &p_clips, const CrowdStanceSet &p_stance, std::span<const CrowdTeam> p_teams,
		uint32_t p_soldiers, const CrowdConfig &p_config) {
	// S3's checks and setup (moved here by R5; unchanged apart from the weapons and the buffer sizes).
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
	uint32_t max_weapon_binds = 0;
	bool any_weapons = false;
	auto check_binds = [&](const SkinBinds *p_binds, const std::string &p_where) -> core::Status {
		if (p_binds == nullptr || p_binds->size() == 0 || p_binds->size() > kMaxPaletteBinds) {
			return core::make_error(p_where + ": " + std::to_string(p_binds == nullptr ? 0 : p_binds->size()) + " binds (1.." + std::to_string(kMaxPaletteBinds) + ")");
		}
		for (const SkinBind &b : p_binds->binds()) {
			if (b.bone < 0 || size_t(b.bone) >= rig.size()) {
				return core::make_error(p_where + ": bind " + b.name + " names bone " + std::to_string(b.bone) + " outside the rig");
			}
		}
		return core::Status::success();
	};
	auto check_layout = [&](const BoneMask *p_joints, std::span<const Xform> p_local, const std::string &p_where) -> core::Status {
		if (p_joints != nullptr || !p_local.empty()) {
			if (p_joints == nullptr || p_joints->size() != rig.size() || p_local.size() != rig.size()) {
				return core::make_error(p_where + ": weapon layout mask / poses are not rig-sized (" + std::to_string(rig.size()) + ")");
			}
		}
		return core::Status::success();
	};
	std::vector<Impl::M2a::Team> m2a_teams(p_teams.size());
	for (size_t t = 0; t < p_teams.size(); ++t) {
		const CrowdTeam &in = p_teams[t];
		const std::string where = "crowd team " + std::to_string(t);
		if (core::Status st = check_binds(in.binds, where); !st.ok()) {
			return st.error();
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
		if (core::Status st = check_layout(in.weapon_joints, in.weapon_local, where); !st.ok()) {
			return st.error();
		}
		if (in.weapon_joints != nullptr) {
			team.weapon_joints = in.weapon_joints;
			team.weapon_local.assign(in.weapon_local.begin(), in.weapon_local.end());
		}
		max_binds = std::max(max_binds, uint32_t(in.binds->size()));
		m.teams.push_back(std::move(team));
		// R5: the team's items.
		if (in.weapons.size() > kMaxCrowdWeapons) {
			return core::make_error(where + ": " + std::to_string(in.weapons.size()) + " weapons (cap " + std::to_string(kMaxCrowdWeapons) + ")");
		}
		for (size_t w = 0; w < in.weapons.size(); ++w) {
			const CrowdWeapon &cw = in.weapons[w];
			const std::string ww = where + " weapon " + std::to_string(w);
			if (core::Status st = check_binds(cw.binds, ww); !st.ok()) {
				return st.error();
			}
			Impl::M2a::Weapon mw;
			mw.binds = cw.binds;
			mw.reload = cw.reload;
			mw.reload_rate = std::isfinite(cw.reload_rate) && cw.reload_rate > 0.0f ? cw.reload_rate : 1.0f;
			if (cw.reload != nullptr) {
				if (cw.reload->mask.size() != rig.size()) {
					return core::make_error(ww + ": reload mask is not rig-sized");
				}
				mw.reload_seconds = cw.reload->tracks.duration_seconds() / mw.reload_rate;
			}
			if (core::Status st = check_layout(cw.weapon_joints, cw.weapon_local, ww); !st.ok()) {
				return st.error();
			}
			if (cw.weapon_joints != nullptr) {
				mw.weapon_joints = cw.weapon_joints;
				mw.weapon_local.assign(cw.weapon_local.begin(), cw.weapon_local.end());
			}
			if (cw.muzzle_joint < -1 || cw.muzzle_joint >= int32_t(rig.size())) {
				return core::make_error(ww + ": muzzle joint " + std::to_string(cw.muzzle_joint) + " outside the rig");
			}
			if (!finite_xform(cw.muzzle_offset)) {
				return core::make_error(ww + ": the muzzle offset is not finite");
			}
			mw.muzzle_joint = cw.muzzle_joint;
			mw.muzzle_offset = cw.muzzle_offset;
			max_weapon_binds = std::max(max_weapon_binds, uint32_t(cw.binds->size()));
			m2a_teams[t].weapons.push_back(std::move(mw));
			any_weapons = true;
		}
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
		// R5: the palette of a soldier that was never evaluated is empty (S3 sized it from team 0's binds, wrong for a
		// team with another bind count); its evaluation sets its own team's count.
		s.binds = { 0, 0 };
	}
	if (!any_weapons && p_stance.pose == nullptr && p_stance.moves == nullptr) {
		return e; // S3's evaluator
	}

	// The M2a state.
	std::unique_ptr<Impl::M2a, Impl::M2aDeleter> x(new Impl::M2a());
	x->teams = std::move(m2a_teams);
	x->death = p_stance.death;
	x->identity = identity_pose(rig);
	core::ant::ClipPose scratch;
	{
		Impl::M2a::Stance &stand = x->stances[0];
		stand.idle = &m.idle;
		stand.aim_up = &m.aim_up;
		stand.aim_down = &m.aim_down;
		stand.up_mask = m.aim_up_mask;
		stand.down_mask = m.aim_down_mask;
		stand.up_full_deg = p_config.aim_up_full_pitch_deg;
		stand.down_full_deg = p_config.aim_down_full_pitch_deg;
		stand.loco = p_clips.loco;
		static constexpr std::array<CrowdClip, kLocoClips> kStandCycle{ CrowdClip::Idle, CrowdClip::RunFwd, CrowdClip::RunBwd, CrowdClip::RunLeft, CrowdClip::RunRight };
		for (size_t c = 1; c < kLocoClips; ++c) {
			stand.cycle[c] = &p_clips.get(kStandCycle[c]);
		}
	}
	for (size_t st = 1; st < 3; ++st) {
		Impl::M2a::Stance &d = x->stances[st];
		d = x->stances[0]; // the stand clips until the stance's own are found
		d.idle = &m.idle;
		d.idle_fallback = true;
		d.moves_fallback = true;
	}
	if (p_stance.pose != nullptr) {
		const ServerPoseClips &sp = *p_stance.pose;
		if (!same_rig(sp.rig, rig)) {
			return core::make_error("crowd: the stance clips' rig (" + std::to_string(sp.rig.size()) + " joints) is not the crowd rig");
		}
		x->stance_set = true;
		const std::array<StanceAim, core::gameplay::kCapsuleStances> aim = server_pose_stance_aim(sp, p_config);
		static constexpr ServerPoseClip kIdle[3] = { ServerPoseClip::StandIdle, ServerPoseClip::CrouchIdle, ServerPoseClip::ProneIdle };
		static constexpr ServerPoseClip kUp[3] = { ServerPoseClip::StandAimUp, ServerPoseClip::CrouchAimUp, ServerPoseClip::ProneAimUp };
		static constexpr ServerPoseClip kDown[3] = { ServerPoseClip::StandAimDown, ServerPoseClip::CrouchAimDown, ServerPoseClip::ProneAimDown };
		for (size_t st = 1; st < 3; ++st) {
			Impl::M2a::Stance &d = x->stances[st];
			if (sp.present[size_t(kIdle[st])]) {
				const CrowdClipData &c = sp.get(kIdle[st]);
				if (sample_pose(rig, c.clip, c.tracks, 0.0f, d.idle_own, scratch)) {
					d.idle_fallback = false;
				}
			}
			// The stance's aim: lc's clips and weights where present (the capsule table's), else the stand aim.
			if (sp.present[size_t(kUp[st])] && additive_delta(rig, sp.get(kUp[st]), d.aim_up_own, scratch)) {
				d.up_mask = &sp.get(kUp[st]).mask;
			} else {
				d.aim_up_own = m.aim_up;
			}
			if (sp.present[size_t(kDown[st])] && additive_delta(rig, sp.get(kDown[st]), d.aim_down_own, scratch)) {
				d.down_mask = &sp.get(kDown[st]).mask;
			} else {
				d.aim_down_own = m.aim_down;
			}
			d.up_full_deg = aim[st].up_full_deg;
			d.down_full_deg = aim[st].down_full_deg;
		}
		for (size_t r = 0; r < core::gameplay::kCapsuleTransitions; ++r) {
			const ServerPoseClip role = ServerPoseClip(size_t(ServerPoseClip::StandToCrouch) + r);
			if (sp.present[size_t(role)] && sp.get(role).tracks.end_frame > 0) {
				x->transitions[r] = &sp.get(role);
			}
		}
	}
	if (p_stance.moves != nullptr) {
		const CrowdMoveClips &mv = *p_stance.moves;
		static constexpr CrowdMoveClip kCycle[kCrowdMoveStances][kLocoClips] = {
			{ CrowdMoveClip::Count, CrowdMoveClip::SneakFwd, CrowdMoveClip::SneakBwdLeft, CrowdMoveClip::SneakLeft, CrowdMoveClip::SneakRight },
			{ CrowdMoveClip::Count, CrowdMoveClip::CrawlFwd, CrowdMoveClip::CrawlBwd, CrowdMoveClip::CrawlLeft, CrowdMoveClip::CrawlRight },
		};
		for (size_t ms = 0; ms < kCrowdMoveStances; ++ms) {
			Impl::M2a::Stance &d = x->stances[ms + 1];
			if (!mv.moves_ok[ms]) {
				continue;
			}
			d.loco = mv.loco[ms];
			d.loco[size_t(LocoClip::Idle)] = LocoClipInfo{};
			for (size_t c = 1; c < kLocoClips; ++c) {
				d.cycle[c] = &mv.get(kCycle[ms][c]);
			}
			d.cycle_bwd2 = ms == 0 ? &mv.get(CrowdMoveClip::SneakBwdRight) : nullptr;
			d.moves_fallback = false;
		}
		static constexpr CrowdMoveClip kDeath[3] = { CrowdMoveClip::DeathStand, CrowdMoveClip::DeathCrouch, CrowdMoveClip::DeathProne };
		for (size_t st = 0; st < 3; ++st) {
			x->stances[st].death = mv.has(kDeath[st]) && mv.get(kDeath[st]).tracks.end_frame > 0 ? &mv.get(kDeath[st]) : nullptr;
		}
		const CrowdClipData &fire = mv.get(CrowdMoveClip::CrouchFire);
		if (mv.has(CrowdMoveClip::CrouchFire) && fire.mask.size() == rig.size() && fire.tracks.end_frame > 0) {
			x->crouch_fire = &fire;
		}
	}
	// The pointers into the Stance entries are set last: the entries were copied above.
	for (size_t st = 0; st < 3; ++st) {
		Impl::M2a::Stance &d = x->stances[st];
		if (st == 0) {
			continue;
		}
		d.idle = d.idle_fallback ? &m.idle : &d.idle_own;
		if (x->stance_set) {
			d.aim_up = &d.aim_up_own;
			d.aim_down = &d.aim_down_own;
		}
	}
	x->soldiers.resize(p_soldiers);
	for (Impl::M2a::Soldier &xs : x->soldiers) {
		for (int k = 0; k < 2; ++k) {
			xs.weapon_palette[size_t(k)].assign(max_weapon_binds, Mat34{});
		}
		xs.extra = rest_pose(rig);
		xs.extra2 = rest_pose(rig);
		xs.alive = rest_pose(rig);
	}
	m.m2a = std::move(x);
	return e;
}

} // namespace bf3::anim
