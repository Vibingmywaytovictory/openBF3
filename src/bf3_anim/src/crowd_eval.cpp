#include "bf3/anim/crowd_eval.h"

#include "bf3/core/math.h"
#include "bf3/core/util/finite.h"
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

constexpr float kReloadRampSeconds = 0.15f;

using bf3::core::finite;

float stagger_of(uint32_t p_i) {
	const double v = double(p_i) * 0.6180339887498949;
	return float(v - std::floor(v));
}

}

struct CrowdEvaluator::Impl {
	struct Team {
		const SkinBinds *binds = nullptr;
		const CrowdClipData *reload = nullptr;
		float reload_rate = 1.0f;
		float reload_seconds = 0.0f;
		const BoneMask *weapon_joints = nullptr;
		std::vector<Xform> weapon_local;
	};

	struct Soldier {
		CrowdSoldierInput input;
		bool reload_pending = false;
		float reload_t = -1.0f;
		float phase = 0.0f;
		double clock = 0.0;
		double last_eval_clock = 0.0;
		int64_t lod_bucket = INT64_MIN;
		bool ever = false;

		std::array<std::vector<Mat34>, 2> palette;
		std::array<std::vector<Xform>, 2> model;
		std::array<uint32_t, 2> binds{};
		uint8_t front = 0;
		bool evaluated = false;
		bool changed = false;

		std::array<LocalPose, 3> sample;
		LocalPose pose;
		core::ant::ClipPose scratch;

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
	LocalPose idle;
	LocalPose aim_up, aim_down;
	const BoneMask *aim_up_mask = nullptr;
	const BoneMask *aim_down_mask = nullptr;
	double dt = 0.0;
	uint64_t bad_inputs = 0;
	CrowdFrameStats stats;
	bool job = false;
	Clock::time_point kick_time{};

	void evaluate(uint32_t p_i);
	void publish(double p_blocked);
	void prepare_job();

	struct Combat;
	struct CombatDeleter {
		void operator()(Combat *p_combat) const;
	};
	std::unique_ptr<Combat, CombatDeleter> combat;
	void evaluate_combat(uint32_t p_i);
};

void CrowdEvaluator::Impl::prepare_job() {
	for (Soldier &s : soldiers) {
		s.evaluated = false;
	}
	kick_time = Clock::now();
	job = true;
}

void CrowdEvaluator::Impl::evaluate(uint32_t p_i) {
	if (combat) {
		evaluate_combat(p_i);
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

	if (config.temporal_lod && s.ever) {
		const float interval = temporal_lod_interval(config.lod, s.input.camera_distance);
		bool due = true;
		if (interval < 0.0f) {
			due = false;
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

	if (clips->root_joint >= 0 && size_t(clips->root_joint) < s.pose.joints.size()) {
		s.pose.joints[size_t(clips->root_joint)].t = rig.bone(size_t(clips->root_joint)).rest.t;
	}

	if (team.weapon_joints != nullptr && !team.weapon_local.empty()) {
		const size_t m = std::min(s.pose.joints.size(), team.weapon_joints->size());
		for (size_t j = 0; j < m; ++j) {
			if (team.weapon_joints->weight[j] > 0.0f) {
				s.pose.joints[j] = team.weapon_local[j];
			}
		}
	}

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

	const float pitch = s.input.pitch;
	const bool up = pitch > 0.0f;
	const float full_deg = up ? config.aim_up_full_pitch_deg : config.aim_down_full_pitch_deg;
	if (pitch != 0.0f && full_deg > 0.0f) {
		const float aw = std::min(1.0f, std::fabs(pitch) / (full_deg * core::kDegToRadF));
		const BoneMask *mask = up ? aim_up_mask : aim_down_mask;
		if (mask != nullptr && aw > 0.0f) {
			apply_additive(s.pose, up ? aim_up : aim_down, *mask, aw, config.aim_order);
			s.aiming = true;
		}
	}

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

	return make(p_clips, CrowdStanceSet{}, p_teams, p_soldiers, p_config);
}

uint32_t CrowdEvaluator::soldiers() const {
	return uint32_t(impl_->soldiers.size());
}

void CrowdEvaluator::set_inputs(std::span<const CrowdSoldierInput> p_inputs, double p_dt) {
	Impl &m = *impl_;
	if (m.job) {
		return;
	}
	if (std::isfinite(p_dt) && p_dt >= 0.0 && p_dt < 1.0e6) {
		m.dt = p_dt;
	} else {
		++m.bad_inputs;
	}
	const size_t n = std::min(p_inputs.size(), m.soldiers.size());
	for (size_t i = 0; i < n; ++i) {
		Impl::Soldier &s = m.soldiers[i];
		const CrowdSoldierInput &in = p_inputs[i];
		CrowdSoldierInput next = in;
		if (!finite(in.position)) {
			next.position = s.input.position;
			++m.bad_inputs;
		}
		if (!finite(in.velocity)) {
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

}

#include "bf3/anim/server_pose.h"

namespace bf3::anim {

namespace {

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
	return std::isfinite(p_x.q.x) && std::isfinite(p_x.q.y) && std::isfinite(p_x.q.z) && std::isfinite(p_x.q.w) &&
			finite(p_x.t);
}

}

struct CrowdMoveRun;
struct CrowdMoveRunDeleter {
	void operator()(CrowdMoveRun *p_run) const;
};
struct CrowdMoveStats {
	uint32_t sampled = 0, moving = 0;
	bool idle_mix = false;
};
std::unique_ptr<CrowdMoveRun, CrowdMoveRunDeleter> crowd_move_make(const CrowdMoveGroupSet &p_set, const Skeleton &p_rig, uint32_t p_soldiers);
bool crowd_move_pose(CrowdMoveRun &r_run, uint32_t p_i, size_t p_stance, const CrowdSoldierInput &p_input, float p_step, const LocalPose &p_idle,
		const Skeleton &p_rig, float &r_phase, core::ant::ClipPose &r_scratch, LocalPose &r_out, CrowdMoveStats &r_stats);

struct CrowdEvaluator::Impl::Combat {
	struct Weapon {
		const SkinBinds *binds = nullptr;
		const CrowdThrowClips *throw_clips = nullptr;
		std::array<std::array<BoneMask, 2>, 2> throw_masks;
		const CrowdClipData *reload = nullptr;
		float reload_rate = 1.0f;
		float reload_seconds = 0.0f;
		const BoneMask *weapon_joints = nullptr;
		std::vector<Xform> weapon_local;
		int32_t muzzle_joint = -1;
		Xform muzzle_offset = xform_identity();
	};
	struct Team {
		std::vector<Weapon> weapons;
	};

	struct Stance {
		const LocalPose *idle = nullptr;
		LocalPose idle_own;
		bool idle_fallback = false;
		const LocalPose *aim_up = nullptr, *aim_down = nullptr;
		LocalPose aim_up_own, aim_down_own;
		const BoneMask *up_mask = nullptr, *down_mask = nullptr;
		float up_full_deg = 0.0f, down_full_deg = 0.0f;
		std::array<LocoClipInfo, kLocoClips> loco{};
		std::array<const CrowdClipData *, kLocoClips> cycle{};
		const CrowdClipData *cycle_bwd2 = nullptr;
		bool moves_fallback = false;
		const CrowdClipData *death = nullptr;
	};
	struct Soldier {
		std::array<std::vector<Mat34>, 2> weapon_palette;
		std::array<uint32_t, 2> weapon_binds{};
		std::array<uint8_t, 2> weapon{ kNoCrowdWeapon, kNoCrowdWeapon };
		std::array<Xform, 2> muzzle{ xform_identity(), xform_identity() };
		std::array<bool, 2> muzzle_ok{};
		uint8_t held = kNoCrowdWeapon;
		uint8_t fire_seq = 0;
		bool fire_seen = false;
		double fire_since = -1.0e9;
		bool holding = false;
		double hold_since = -1.0e9;
		bool held_into_release = false;
		bool dead = false;
		double dead_since = 0.0;
		uint8_t death_stance = 0;
		bool hidden = false;
		bool hide_counted = false;
		bool rising = false;
		double rise_since = 0.0;
		LocalPose corpse;
		uint64_t revives = 0;
		LocalPose extra, extra2, alive;

		uint8_t stance = 0;
		bool transition = false, moving = false, firing = false, throwing = false, dying = false, wrote_weapon = false,
			 wrote_muzzle = false;

		uint64_t fallbacks = 0, bad_weapon = 0, bad_stance = 0, cancelled = 0, deaths = 0, hidden_count = 0;
	};

	std::vector<Team> teams;
	std::array<Stance, 3> stances;
	bool stance_set = false;
	std::array<const CrowdClipData *, core::gameplay::kCapsuleTransitions> transitions{};
	const CrowdClipData *crouch_fire = nullptr;
	CrowdDeathStyle death = kCrowdDeathDefault;
	float corpse_seconds = kCrowdDeathHideSeconds;
	LocalPose identity;
	std::vector<Soldier> soldiers;
	std::unique_ptr<CrowdMoveRun, CrowdMoveRunDeleter> move_run;
};

void CrowdEvaluator::Impl::CombatDeleter::operator()(Combat *p_combat) const {
	delete p_combat;
}

core::Result<std::unique_ptr<CrowdEvaluator>> CrowdEvaluator::make(const CrowdClipSet &p_clips, const CrowdStanceSet &p_stance, std::span<const CrowdTeam> p_teams,
		uint32_t p_soldiers, const CrowdConfig &p_config) {

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
	std::vector<Impl::Combat::Team> combat_teams(p_teams.size());
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

		if (in.weapons.size() > kMaxCrowdWeapons) {
			return core::make_error(where + ": " + std::to_string(in.weapons.size()) + " weapons (cap " + std::to_string(kMaxCrowdWeapons) + ")");
		}
		for (size_t w = 0; w < in.weapons.size(); ++w) {
			const CrowdWeapon &cw = in.weapons[w];
			const std::string ww = where + " weapon " + std::to_string(w);
			if (core::Status st = check_binds(cw.binds, ww); !st.ok()) {
				return st.error();
			}
			Impl::Combat::Weapon mw;
			mw.binds = cw.binds;
			if (cw.throw_clips != nullptr) {
				const CrowdThrowClips &tc = *cw.throw_clips;
				if (tc.in.mask.size() != rig.size() || tc.release.mask.size() != rig.size() ||
						tc.moving.size() != rig.size() || tc.still.size() != rig.size()) {
					return core::make_error(ww + ": throw clip or mask is not rig-sized");
				}
				if (!(tc.in_seconds > 0.0f) || !(tc.release_seconds > 0.0f) || !std::isfinite(tc.in_seconds) ||
						!std::isfinite(tc.release_seconds)) {
					return core::make_error(ww + ": throw clip lengths are not positive");
				}
				mw.throw_clips = cw.throw_clips;
				for (size_t c = 0; c < 2; ++c) {
					for (size_t a = 0; a < 2; ++a) {
						const BoneMask &clip_mask = c == 0 ? tc.in.mask : tc.release.mask;
						const BoneMask &area = a == 0 ? tc.still : tc.moving;
						BoneMask &dst = mw.throw_masks[c][a];
						dst.weight.resize(rig.size());
						for (size_t j = 0; j < rig.size(); ++j) {
							dst.weight[j] = clip_mask.weight[j] * area.weight[j];
						}
					}
				}
			}
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
			combat_teams[t].weapons.push_back(std::move(mw));
			any_weapons = true;
		}
	}

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
				continue;
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

		s.binds = { 0, 0 };
	}
	if (!any_weapons && p_stance.pose == nullptr && p_stance.moves == nullptr && p_stance.move_groups == nullptr) {
		return e;
	}

	std::unique_ptr<Impl::Combat, Impl::CombatDeleter> x(new Impl::Combat());
	x->teams = std::move(combat_teams);
	x->death = p_stance.death;
	if (!std::isfinite(p_stance.corpse_seconds) || p_stance.corpse_seconds < 0.0f || p_stance.corpse_seconds > kCrowdMaxCorpseSeconds) {
		return core::make_error("crowd: corpse_seconds " + std::to_string(p_stance.corpse_seconds) + " outside [0, 3600]");
	}
	x->corpse_seconds = p_stance.corpse_seconds;
	x->identity = identity_pose(rig);
	core::ant::ClipPose scratch;
	{
		Impl::Combat::Stance &stand = x->stances[0];
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
		Impl::Combat::Stance &d = x->stances[st];
		d = x->stances[0];
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
			Impl::Combat::Stance &d = x->stances[st];
			if (sp.present[size_t(kIdle[st])]) {
				const CrowdClipData &c = sp.get(kIdle[st]);
				if (sample_pose(rig, c.clip, c.tracks, 0.0f, d.idle_own, scratch)) {
					d.idle_fallback = false;
				}
			}

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
			Impl::Combat::Stance &d = x->stances[ms + 1];
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

	for (size_t st = 0; st < 3; ++st) {
		Impl::Combat::Stance &d = x->stances[st];
		if (st == 0) {
			continue;
		}
		d.idle = d.idle_fallback ? &m.idle : &d.idle_own;
		if (x->stance_set) {
			d.aim_up = &d.aim_up_own;
			d.aim_down = &d.aim_down_own;
		}
	}
	if (p_stance.move_groups != nullptr) {
		x->move_run = crowd_move_make(*p_stance.move_groups, rig, p_soldiers);
	}
	x->soldiers.resize(p_soldiers);
	for (Impl::Combat::Soldier &xs : x->soldiers) {
		for (int k = 0; k < 2; ++k) {
			xs.weapon_palette[size_t(k)].assign(max_weapon_binds, Mat34{});
		}
		xs.extra = rest_pose(rig);
		xs.extra2 = rest_pose(rig);
		xs.alive = rest_pose(rig);
	}
	m.combat = std::move(x);
	return e;
}

void CrowdEvaluator::Impl::evaluate_combat(uint32_t p_i) {
	Soldier &s = soldiers[p_i];
	Combat &x = *combat;
	Combat::Soldier &xs = x.soldiers[p_i];
	s.skipped_lod = s.hidden = s.two_cycle = s.idle_mix = s.reloading = s.aiming = false;
	s.loco_clips = 0;
	xs.transition = xs.moving = xs.firing = xs.throwing = xs.dying = xs.wrote_weapon = xs.wrote_muzzle = false;
	s.tier = int8_t(temporal_lod_tier(config.lod, s.input.camera_distance));
	s.clock += dt;
	const auto finish = [&] { s.done_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); };

	if (s.input.dead && !xs.dead) {
		xs.dead = true;
		xs.dead_since = s.clock;
		xs.death_stance = uint8_t(std::min<uint32_t>(s.input.target_pose, 2));
		xs.hide_counted = false;
		++xs.deaths;
		xs.rising = false;
	} else if (!s.input.dead && xs.dead) {
		xs.dead = false;

		xs.rising = s.input.revived && s.pose.joints.size() == clips->rig.size();
		if (xs.rising) {
			xs.rise_since = s.clock;
			xs.corpse = s.pose;
			++xs.revives;
		}
	}
	if (s.input.fire_seq != xs.fire_seq) {
		if (xs.fire_seen) {
			xs.fire_since = s.clock;
			xs.held_into_release = xs.holding;
		}
		xs.fire_seq = s.input.fire_seq;
	}
	if (s.input.holding && !xs.holding) {
		xs.hold_since = s.clock;
	}
	xs.holding = s.input.holding;
	xs.fire_seen = true;
	const double dead_age = xs.dead ? s.clock - xs.dead_since : 0.0;
	xs.hidden = xs.dead && dead_age >= double(x.corpse_seconds);
	if (xs.hidden && !xs.hide_counted) {
		xs.hide_counted = true;
		++xs.hidden_count;
	}
	if ((s.input.flags & crowd_flags::kHidden) != 0 || xs.hidden) {
		s.hidden = true;
		finish();
		return;
	}

	if (config.temporal_lod && s.ever && !xs.dead && !xs.rising) {
		const float interval = temporal_lod_interval(config.lod, s.input.camera_distance);
		bool due = true;
		if (interval < 0.0f) {
			due = false;
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
	const Combat::Team &xteam = x.teams[team_index];
	const Skeleton &rig = clips->rig;

	uint8_t widx = kNoCrowdWeapon;
	const Combat::Weapon *weapon = nullptr;
	if (!xteam.weapons.empty()) {
		widx = s.input.weapon;
		if (widx >= xteam.weapons.size()) {
			++xs.bad_weapon;
			widx = 0;
		}
		weapon = &xteam.weapons[widx];
	}
	const BoneMask *weapon_joints = weapon != nullptr ? weapon->weapon_joints : team.weapon_joints;
	const std::vector<Xform> &weapon_local = weapon != nullptr ? weapon->weapon_local : team.weapon_local;
	const CrowdClipData *reload = weapon != nullptr ? weapon->reload : team.reload;
	const float reload_rate = weapon != nullptr ? weapon->reload_rate : team.reload_rate;
	const float reload_seconds = weapon != nullptr ? weapon->reload_seconds : team.reload_seconds;
	if (widx != xs.held) {
		if (xs.held != kNoCrowdWeapon && s.reload_t >= 0.0f) {
			s.reload_t = -1.0f;
			++xs.cancelled;
		}
		xs.held = widx;
	}

	uint8_t from = s.input.pose, to = s.input.target_pose;
	if (from > 2 || to > 2) {
		++xs.bad_stance;
		from = std::min<uint8_t>(from, 2);
		to = std::min<uint8_t>(to, 2);
	}
	float tr = s.input.transition;
	if (!std::isfinite(tr)) {
		++xs.bad_stance;
		tr = 0.0f;
	}
	tr = std::clamp(tr, 0.0f, 1.0f);
	const bool in_transition = from != to;
	const uint8_t stance = in_transition ? from : to;
	xs.stance = to;
	const Combat::Stance &aim_stance = x.stances[stance];

	auto loco_blend = [&](const Combat::Stance &p_st, LocalPose &r_out) {
		if (x.move_run) {
			CrowdMoveStats ps;
			if (crowd_move_pose(*x.move_run, p_i, size_t(&p_st - x.stances.data()), s.input, step, *p_st.idle, rig, s.phase, s.scratch, r_out, ps)) {
				s.loco_clips = uint8_t(std::min<uint32_t>(ps.sampled, 255));
				s.two_cycle = ps.moving >= 2;
				s.idle_mix = ps.idle_mix;
				xs.moving = ps.moving >= 1;
				return;
			}
		}
		const float yaw = s.input.yaw;
		const float cy = std::cos(yaw), sy = std::sin(yaw);
		const Vec3 v = s.input.velocity;
		const Vec3 local{ v.x * cy - v.z * sy, 0.0f, v.x * sy + v.z * cy };
		const LocoWeights w = locomotion_weights(p_st.loco, local, config.loco);
		if (w.cycle_seconds > 0.0f) {
			s.phase = advance_phase(s.phase, step, w.cycle_seconds);
		}
		std::array<const LocalPose *, 3> inputs{};
		std::array<float, 3> weights{};
		size_t n = 0;
		uint32_t cycles = 0;
		for (size_t c = 0; c < kLocoClips && n < 3; ++c) {
			if (!(w.weight[c] > 0.0f)) {
				continue;
			}
			if (c == size_t(LocoClip::Idle)) {
				inputs[n] = p_st.idle;
			} else {
				const CrowdClipData &clip = *p_st.cycle[c];
				LocalPose &dst = s.sample[cycles];
				if (c == size_t(LocoClip::Bwd) && p_st.cycle_bwd2 != nullptr) {

					const CrowdClipData &c2 = *p_st.cycle_bwd2;
					sample_pose(rig, clip.clip, clip.tracks, s.phase * float(clip.tracks.end_frame), xs.extra, s.scratch);
					sample_pose(rig, c2.clip, c2.tracks, s.phase * float(c2.tracks.end_frame), xs.extra2, s.scratch);
					blend_poses(xs.extra, xs.extra2, 0.5f, dst);
				} else {
					sample_pose(rig, clip.clip, clip.tracks, s.phase * float(clip.tracks.end_frame), dst, s.scratch);
				}
				inputs[n] = &dst;
				++cycles;
			}
			weights[n] = w.weight[c];
			++n;
		}
		s.loco_clips = uint8_t(n);
		s.two_cycle = cycles >= 2;
		s.idle_mix = w.weight[size_t(LocoClip::Idle)] > 0.0f && cycles >= 1;
		xs.moving = cycles >= 1;
		if (n == 1) {
			r_out = *inputs[0];
		} else {
			blend_poses(inputs.data(), weights.data(), n, r_out);
		}
	};
	auto stance_pose = [&](LocalPose &r_out) {
		if (in_transition) {
			xs.transition = true;
			const int row = transition_row(from, to);
			const CrowdClipData *clip = row >= 0 ? x.transitions[size_t(row)] : nullptr;
			if (clip != nullptr) {
				sample_pose(rig, clip->clip, clip->tracks, tr * float(clip->tracks.end_frame), r_out, s.scratch);
			} else {

				++xs.fallbacks;
				blend_poses(*x.stances[from].idle, *x.stances[to].idle, tr, r_out);
			}
			return;
		}
		const Combat::Stance &st = x.stances[to];
		loco_blend(st, r_out);

		if (to != 0 && (st.idle_fallback || (st.moves_fallback && xs.moving))) {
			++xs.fallbacks;
		}
	};
	if (xs.dead) {
		xs.dying = true;
		const Combat::Stance &ds = x.stances[xs.death_stance];
		if (x.death == CrowdDeathStyle::Clip && ds.death != nullptr) {
			const float frame = std::min(float(dead_age) * ds.death->tracks.fps, float(ds.death->tracks.end_frame));
			sample_pose(rig, ds.death->clip, ds.death->tracks, frame, s.pose, s.scratch);
		} else {

			stance_pose(xs.alive);
			const float w = std::min(1.0f, float(dead_age) / kCrowdDeathBlendSeconds);
			blend_poses(xs.alive, *x.stances[2].idle, w, s.pose);
		}
	} else if (xs.rising && s.clock - xs.rise_since < double(kCrowdReviveBlendSeconds)) {
		stance_pose(xs.alive);
		blend_poses(xs.corpse, xs.alive, float((s.clock - xs.rise_since) / double(kCrowdReviveBlendSeconds)), s.pose);
	} else {
		xs.rising = false;
		stance_pose(s.pose);
	}

	if (clips->root_joint >= 0 && size_t(clips->root_joint) < s.pose.joints.size()) {
		s.pose.joints[size_t(clips->root_joint)].t = rig.bone(size_t(clips->root_joint)).rest.t;
	}

	if (weapon_joints != nullptr && !weapon_local.empty()) {
		const size_t m = std::min(s.pose.joints.size(), weapon_joints->size());
		for (size_t j = 0; j < m; ++j) {
			if (weapon_joints->weight[j] > 0.0f) {
				s.pose.joints[j] = weapon_local[j];
			}
		}
	}

	if (xs.dead) {
		s.reload_pending = false;
		s.reload_t = -1.0f;
	}
	if (s.reload_pending) {
		s.reload_pending = false;
		if (s.reload_t < 0.0f && reload != nullptr) {
			s.reload_t = 0.0f;
		}
	} else if (s.reload_t >= 0.0f) {
		s.reload_t += step;
	}
	if (s.reload_t >= 0.0f && reload != nullptr) {
		if (s.reload_t >= reload_seconds) {
			s.reload_t = -1.0f;
		} else {
			const float rw = std::min({ 1.0f, s.reload_t / kReloadRampSeconds, (reload_seconds - s.reload_t) / kReloadRampSeconds });
			const CrowdClipData &clip = *reload;
			LocalPose &dst = s.sample[2];
			sample_pose(rig, clip.clip, clip.tracks, s.reload_t * reload_rate * clip.tracks.fps, dst, s.scratch);
			override_masked(s.pose, dst, clip.mask, rw);
			s.reloading = rw > 0.0f;
		}
	}

	float kick = 0.0f;
	const double fire_age = s.clock - xs.fire_since;
	const CrowdThrowClips *throw_clips = weapon != nullptr ? weapon->throw_clips : nullptr;
	if (throw_clips != nullptr && !xs.dead) {
		const size_t area = xs.moving ? 1 : 0;
		const CrowdClipData *clip = nullptr;
		const BoneMask *mask = nullptr;
		float t = 0.0f, w = 0.0f;
		if (fire_age >= 0.0 && fire_age < double(throw_clips->release_seconds)) {
			clip = &throw_clips->release;
			mask = &weapon->throw_masks[1][area];
			t = float(fire_age);
			const float in_w = xs.held_into_release ? 1.0f : t / kReloadRampSeconds;
			w = std::min({ 1.0f, in_w, (throw_clips->release_seconds - t) / kReloadRampSeconds });
		} else if (xs.holding) {
			clip = &throw_clips->in;
			mask = &weapon->throw_masks[0][area];
			const float age = float(s.clock - xs.hold_since);
			t = std::min(age, throw_clips->in_seconds);
			w = std::min(1.0f, age / kReloadRampSeconds);
		}
		if (clip != nullptr && w > 0.0f &&
				sample_pose(rig, clip->clip, clip->tracks, t * clip->tracks.fps, xs.extra, s.scratch)) {
			override_masked(s.pose, xs.extra, *mask, w);
			xs.throwing = true;
		}
	} else if (!xs.dead && fire_age >= 0.0) {
		if (!in_transition && to == 1 && x.crouch_fire != nullptr) {
			const CrowdClipData &clip = *x.crouch_fire;
			const float dur = clip.tracks.duration_seconds();
			if (fire_age < double(dur)) {
				const float frame = float(fire_age) * clip.tracks.fps;
				if (clip.additive) {
					xs.extra = x.identity;
					if (core::ant::sample_clip(clip.clip, frame, s.scratch)) {
						apply_clip_pose(clip.tracks, s.scratch, xs.extra);
						apply_additive(s.pose, xs.extra, clip.mask, 1.0f, config.aim_order);
						xs.firing = true;
					}
				} else if (sample_pose(rig, clip.clip, clip.tracks, frame, xs.extra, s.scratch)) {
					const float fw = std::min({ 1.0f, float(fire_age) / kReloadRampSeconds, (dur - float(fire_age)) / kReloadRampSeconds });
					override_masked(s.pose, xs.extra, clip.mask, std::max(0.0f, fw));
					xs.firing = true;
				}
			}
		} else if (fire_age < double(kCrowdFireKickSeconds)) {
			kick = kCrowdFireKickDeg * core::kDegToRadF * (1.0f - float(fire_age) / kCrowdFireKickSeconds);
			xs.firing = true;
		}
	}

	if (!xs.dead) {
		const float pitch = kick != 0.0f ? s.input.pitch + kick : s.input.pitch;
		const bool up = pitch > 0.0f;
		const float full_deg = up ? aim_stance.up_full_deg : aim_stance.down_full_deg;
		if (pitch != 0.0f && full_deg > 0.0f) {
			const float aw = std::min(1.0f, std::fabs(pitch) / (full_deg * core::kDegToRadF));
			const BoneMask *mask = up ? aim_stance.up_mask : aim_stance.down_mask;
			if (mask != nullptr && aw > 0.0f) {
				apply_additive(s.pose, up ? *aim_stance.aim_up : *aim_stance.aim_down, *mask, aw, config.aim_order);
				s.aiming = true;
			}
		}
	}

	const uint8_t back = uint8_t(s.front ^ 1);
	model_pose(rig, s.pose, s.model[back]);
	skin_matrices(s.model[back], team.binds->binds(), s.palette[back]);
	s.binds[back] = uint32_t(team.binds->size());
	xs.weapon[back] = widx;
	xs.muzzle_ok[back] = false;
	xs.weapon_binds[back] = 0;
	if (weapon != nullptr) {
		skin_matrices(s.model[back], weapon->binds->binds(), xs.weapon_palette[back]);
		xs.weapon_binds[back] = uint32_t(weapon->binds->size());
		xs.wrote_weapon = true;
		if (weapon->muzzle_joint >= 0 && size_t(weapon->muzzle_joint) < s.model[back].size()) {
			xs.muzzle[back] = xform_mul(s.model[back][size_t(weapon->muzzle_joint)], weapon->muzzle_offset);
			xs.muzzle_ok[back] = true;
			xs.wrote_muzzle = true;
		}
	}
	s.evaluated = true;
	finish();
}

bool CrowdEvaluator::muzzle_model(uint32_t p_i, Xform &r_model) const {
	const Impl &m = *impl_;
	if (!m.combat || !drawn(p_i)) {
		return false;
	}
	const Impl::Soldier &s = m.soldiers[p_i];
	const Impl::Combat::Soldier &xs = m.combat->soldiers[p_i];
	if (!xs.muzzle_ok[s.front]) {
		return false;
	}
	r_model = xs.muzzle[s.front];
	return true;
}

uint8_t CrowdEvaluator::weapon_of(uint32_t p_i) const {
	const Impl &m = *impl_;
	if (!m.combat || !drawn(p_i)) {
		return kNoCrowdWeapon;
	}
	return m.combat->soldiers[p_i].weapon[m.soldiers[p_i].front];
}

std::span<const Mat34> CrowdEvaluator::weapon_palette(uint32_t p_i) const {
	if (weapon_of(p_i) == kNoCrowdWeapon) {
		return {};
	}
	const Impl &m = *impl_;
	const Impl::Combat::Soldier &xs = m.combat->soldiers[p_i];
	const uint8_t f = m.soldiers[p_i].front;
	return std::span<const Mat34>(xs.weapon_palette[f].data(), std::min<size_t>(xs.weapon_palette[f].size(), xs.weapon_binds[f]));
}

bool CrowdEvaluator::drawn(uint32_t p_i) const {
	const Impl &m = *impl_;
	if (p_i >= m.soldiers.size()) {
		return false;
	}
	const Impl::Soldier &s = m.soldiers[p_i];
	if (!s.ever || (s.input.flags & crowd_flags::kHidden) != 0) {
		return false;
	}
	return !m.combat || !m.combat->soldiers[p_i].hidden;
}

CrowdCombatStats CrowdEvaluator::combat_stats() const {
	CrowdCombatStats r;
	const Impl &m = *impl_;
	if (!m.combat) {
		return r;
	}
	for (size_t i = 0; i < m.soldiers.size(); ++i) {
		const Impl::Soldier &s = m.soldiers[i];
		const Impl::Combat::Soldier &xs = m.combat->soldiers[i];
		r.stance_fallbacks += xs.fallbacks;
		r.bad_weapon += xs.bad_weapon;
		r.bad_stance += xs.bad_stance;
		r.reloads_cancelled += xs.cancelled;
		r.deaths += xs.deaths;
		r.death_hidden += xs.hidden_count;
		r.revives += xs.revives;
		if (!s.changed) {
			continue;
		}
		if (xs.transition) {
			++r.transitions;
		} else if (xs.stance == 0) {
			++r.stand;
		} else if (xs.stance == 1) {
			++r.crouch;
		} else {
			++r.prone;
		}
		r.moving_crouch += !xs.transition && xs.stance == 1 && xs.moving ? 1 : 0;
		r.moving_prone += !xs.transition && xs.stance == 2 && xs.moving ? 1 : 0;
		r.firing += xs.firing ? 1 : 0;
		r.throwing += xs.throwing ? 1 : 0;
		r.dying += xs.dying ? 1 : 0;
		r.weapon_palettes += xs.wrote_weapon ? 1 : 0;
		r.muzzles += xs.wrote_muzzle ? 1 : 0;
	}
	return r;
}

}

namespace bf3::anim {

struct CrowdMoveRun {
	struct Soldier {
		float sprint_w = 0.0f;
		float move_w = 0.0f;
		uint8_t stance = 0xFF;
		std::array<LocalPose, kMaxMoveGroupTerms> term;
		LocalPose group_a, group_b;
	};
	const CrowdMoveGroupSet *set = nullptr;
	std::vector<Soldier> soldiers;
};

void CrowdMoveRunDeleter::operator()(CrowdMoveRun *p_run) const {
	delete p_run;
}

std::unique_ptr<CrowdMoveRun, CrowdMoveRunDeleter> crowd_move_make(const CrowdMoveGroupSet &p_set, const Skeleton &p_rig, uint32_t p_soldiers) {
	std::unique_ptr<CrowdMoveRun, CrowdMoveRunDeleter> run(new CrowdMoveRun());
	run->set = &p_set;
	run->soldiers.resize(p_soldiers);
	for (CrowdMoveRun::Soldier &x : run->soldiers) {
		for (LocalPose &t : x.term) {
			t = rest_pose(p_rig);
		}
		x.group_a = rest_pose(p_rig);
		x.group_b = rest_pose(p_rig);
	}
	return run;
}

namespace {

uint32_t sample_group_pose(const CrowdMoveGroup &p_group, const MoveGroupWeights &p_w, float p_phase, const LocalPose &p_idle,
		std::array<LocalPose, kMaxMoveGroupTerms> &r_term, core::ant::ClipPose &r_scratch, LocalPose &r_out) {
	std::array<const LocalPose *, kMaxMoveGroupTerms> inputs{};
	std::array<float, kMaxMoveGroupTerms> weights{};
	uint32_t n = 0;
	for (uint32_t k = 0; k < p_w.count && k < kMaxMoveGroupTerms; ++k) {
		const MoveGroupTerm &t = p_w.terms[k];
		if (t.clip >= p_group.clips.size() || !(t.weight > 0.0f)) {
			continue;
		}
		const CrowdClipData &c = p_group.clips[t.clip];
		float frame = 0.0f;
		if (p_group.timing[t.clip].seconds > 0.0f) {
			float u = p_phase + t.start_phase;
			u -= std::floor(u);
			frame = u * float(c.tracks.end_frame);
		}
		LocalPose &dst = r_term[n];
		dst = p_idle;
		if (core::ant::sample_clip(c.clip, frame, r_scratch)) {
			apply_clip_pose(c.tracks, r_scratch, dst);
		}
		inputs[n] = &dst;
		weights[n] = t.weight;
		++n;
	}
	if (n == 0) {
		r_out = p_idle;
	} else if (n == 1) {
		r_out = *inputs[0];
	} else {
		blend_poses(inputs.data(), weights.data(), n, r_out);
	}
	return n;
}

float ramp_weight(float p_w, float p_target, float p_step, float p_seconds) {
	const float d = p_seconds > 0.0f ? p_step / p_seconds : 1.0f;
	return p_w < p_target ? std::min(p_target, p_w + d) : std::max(p_target, p_w - d);
}

}

bool crowd_move_pose(CrowdMoveRun &r_run, uint32_t p_i, size_t p_stance, const CrowdSoldierInput &p_input, float p_step, const LocalPose &p_idle,
		const Skeleton &p_rig, float &r_phase, core::ant::ClipPose &r_scratch, LocalPose &r_out, CrowdMoveStats &r_stats) {
	(void)p_rig;
	r_stats = CrowdMoveStats{};
	if (r_run.set == nullptr || p_i >= r_run.soldiers.size() || p_stance > 1) {
		return false;
	}
	const CrowdMoveGroup &main = r_run.set->get(p_stance == 0 ? CrowdMoveGroupRole::Run : CrowdMoveGroupRole::Sneak);
	if (!main.ok) {
		return false;
	}
	const CrowdMoveGroup &sprint_group = r_run.set->get(CrowdMoveGroupRole::Sprint);
	const bool can_sprint = p_stance == 0 && sprint_group.ok;
	CrowdMoveRun::Soldier &x = r_run.soldiers[p_i];
	const float step = std::isfinite(p_step) ? std::clamp(p_step, 0.0f, 1.0f) : 0.0f;

	const float cy = std::cos(p_input.yaw), sy = std::sin(p_input.yaw);
	const Vec3 v = p_input.velocity;
	Vec3 local{ v.x * cy - v.z * sy, 0.0f, v.x * sy + v.z * cy };
	if (!std::isfinite(local.x) || !std::isfinite(local.z)) {
		local = Vec3{};
	}
	const float speed = std::sqrt(local.x * local.x + local.z * local.z);
	const MoveGroupOptions &opt = r_run.set->options;
	const MoveGroupWeights wa = move_group_weights(main.def, main.timing, move_group_x(local, main.x_sign), speed, opt);
	const bool sprinting = can_sprint && p_input.sprinting;
	const float sprint_target = sprinting ? 1.0f : 0.0f;
	const float move_target = (!wa.below_min_speed || sprinting) ? 1.0f : 0.0f;
	if (x.stance != uint8_t(p_stance)) {
		x.stance = uint8_t(p_stance);
		x.sprint_w = sprint_target;
		x.move_w = move_target;
	} else {
		x.sprint_w = ramp_weight(x.sprint_w, sprint_target, step, kCrowdSprintBlendSeconds);
		x.move_w = ramp_weight(x.move_w, move_target, step, kCrowdMoveBlendSeconds);
	}
	if (!can_sprint) {
		x.sprint_w = 0.0f;
	}
	const float sw = x.sprint_w;
	MoveGroupWeights ws;
	if (sw > 0.0f) {
		ws = move_group_weights(sprint_group.def, sprint_group.timing, move_group_x(local, sprint_group.x_sign), speed, opt);
	}

	float cps = wa.cycles_per_second;
	if (sw > 0.0f) {
		const float b = ws.cycles_per_second;
		cps = cps > 0.0f && b > 0.0f ? (1.0f - sw) * cps + sw * b : (b > 0.0f ? b : cps);
	}
	if (x.move_w > 0.0f && cps > 0.0f && std::isfinite(cps)) {
		r_phase = advance_phase(r_phase, step, 1.0f / cps);
	}
	if (!(x.move_w > 0.0f)) {
		r_out = p_idle;
		r_stats.sampled = 1;
		return true;
	}
	uint32_t sampled = 0, moving = 0;
	if (sw >= 1.0f) {
		sampled += sample_group_pose(sprint_group, ws, r_phase, p_idle, x.term, r_scratch, x.group_a);
		moving += ws.moving;
	} else {
		sampled += sample_group_pose(main, wa, r_phase, p_idle, x.term, r_scratch, x.group_a);
		moving += wa.moving;
		if (sw > 0.0f) {
			sampled += sample_group_pose(sprint_group, ws, r_phase, p_idle, x.term, r_scratch, x.group_b);
			moving += ws.moving;
			blend_poses(x.group_a, x.group_b, sw, x.group_a);
		}
	}
	if (x.move_w >= 1.0f) {
		r_out = x.group_a;
	} else {
		blend_poses(p_idle, x.group_a, x.move_w, r_out);
		++sampled;
		r_stats.idle_mix = moving >= 1;
	}
	r_stats.sampled = sampled;
	r_stats.moving = moving;
	return true;
}

}
