#include "audio_ingame_check.h"

#include "game_audio.h"
#include "level_session_game.h"
#include "m2b_check_rows.h"

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/voice_cull.h"
#include "bf3/core/math.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/text.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numbers>

namespace bf3::godot_side {

namespace ba = bf3::audio;

namespace {

bool silent_kind(ba::MixerCutKind p_kind) {
	switch (p_kind) {
		case ba::MixerCutKind::Start:
		case ba::MixerCutKind::Suspend:
		case ba::MixerCutKind::Unsuspend:
		case ba::MixerCutKind::Fade:
		case ba::MixerCutKind::LateBlock:
		case ba::MixerCutKind::CallGap:
		case ba::MixerCutKind::Request:
		case ba::MixerCutKind::Refused:
		case ba::MixerCutKind::Cull:
		case ba::MixerCutKind::Park:
		case ba::MixerCutKind::Coalesce:
		case ba::MixerCutKind::Join:
			return true;
		default:
			return false;
	}
}

bool data_reason(uint8_t p_reason) {
	return p_reason == uint8_t(ba::CullVerdict::Precull) || p_reason == uint8_t(ba::CullVerdict::Scope);
}

std::string reason_text(uint8_t p_reason, std::string_view p_scope) {
	switch (ba::CullVerdict(p_reason)) {
		case ba::CullVerdict::Precull:
			return "below the HDR window";
		case ba::CullVerdict::Scope:
			return "scope " + std::string(p_scope);
		case ba::CullVerdict::Cap:
			return "voice cap";
		default:
			return "reason " + std::to_string(unsigned(p_reason));
	}
}

}

float audio_ingame_clearance(
		const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, const sim::Vec3 &p_feet, float p_yaw) {
	const sim::PoseParams &stand = p_motor.poses[0];
	const sim::Vec3 from{ p_feet[0], p_feet[1] + stand.step_height, p_feet[2] };
	const sim::Vec3 delta{ std::sin(p_yaw) * kAudioIngameRunMeters, 0.0f, std::cos(p_yaw) * kAudioIngameRunMeters };
	const float radius = p_motor.radius * kAudioIngameRunRadiusShare;
	sim::StaticHit hit;
	if (!p_world.sweep_capsule(from, radius, std::max(stand.height - stand.step_height, radius * 2.0f), delta,
				sim::query_mask::kMovement, &hit)) {
		return kAudioIngameRunMeters;
	}
	return std::clamp(hit.fraction, 0.0f, 1.0f) * kAudioIngameRunMeters;
}

float audio_ingame_clearest_yaw(const std::function<float(float)> &p_clearance, float p_from, float &r_clear_m) {
	float yaw = p_from;
	r_clear_m = -1.0f;
	for (uint32_t i = 0; i < kAudioIngameRunYaws; ++i) {
		const float at = p_from + 2.0f * std::numbers::pi_v<float> * float(i) / float(kAudioIngameRunYaws);
		const float clear = p_clearance(at);
		if (clear > r_clear_m) {
			r_clear_m = clear;
			yaw = at;
		}
	}
	return yaw;
}

std::optional<AudioIngameSpot> audio_ingame_open_spot(const sim::CollisionWorld &p_world,
		const sim::MotorParams &p_motor, std::span<const sim::SpawnPoint> p_spawns) {
	std::optional<AudioIngameSpot> best;
	for (const sim::SpawnPoint &p : p_spawns) {
		AudioIngameSpot spot;
		spot.feet = p.position;
		spot.yaw = audio_ingame_clearest_yaw(
				[&](float p_yaw) { return audio_ingame_clearance(p_world, p_motor, p.position, p_yaw); }, 0.0f,
				spot.clear_m);
		if (!best || spot.clear_m > best->clear_m) {
			best = spot;
		}
	}
	return best;
}

std::optional<AudioIngameSpot> audio_ingame_fight_spot(std::span<const present::PresentedSoldier> p_others,
		std::span<const uint8_t> p_slot_team, uint8_t p_own_team, const sim::Vec3 &p_feet) {
	const present::PresentedSoldier *best = nullptr;
	float best_d = 0.0f;
	for (const present::PresentedSoldier &o : p_others) {
		const uint8_t team = o.slot < p_slot_team.size() ? p_slot_team[o.slot] : 0;
		if (team == 0 || team == p_own_team || o.mode == sim::MoveMode::Dead) {
			continue;
		}
		const float d = std::hypot(o.position[0] - p_feet[0], o.position[2] - p_feet[2]);
		if (best == nullptr || d < best_d) {
			best = &o;
			best_d = d;
		}
	}
	if (best == nullptr) {
		return std::nullopt;
	}
	const M2bSeekSpot s = m2b_seek_spot(best->position, best->yaw, kAudioIngameFightMeters);
	AudioIngameSpot spot;
	spot.feet = s.position;
	spot.yaw = s.yaw;
	return spot;
}

bool parse_audio_ingame_phase(std::string_view p_text, AudioIngamePhase &r_phase) {
	if (p_text.empty() || p_text == "mix") {
		r_phase = AudioIngamePhase::Mix;
		return true;
	}
	if (p_text == "pool") {
		r_phase = AudioIngamePhase::Pool;
		return true;
	}
	if (p_text == "steps") {
		r_phase = AudioIngamePhase::Steps;
		return true;
	}
	if (p_text == "flyby") {
		r_phase = AudioIngamePhase::Flyby;
		return true;
	}
	return false;
}

AudioIngameCheck::AudioIngameCheck(std::string p_out_dir, double p_seconds, AudioIngamePhase p_phase) :
		out_dir_(std::move(p_out_dir)),
		seconds_(std::isfinite(p_seconds) && p_seconds > 0.0 ? std::clamp(p_seconds, 5.0, 120.0) : kAudioIngameSeconds),
		kind_(p_phase) {}

AudioIngameCheck::~AudioIngameCheck() {
	if (capture_.is_valid()) {
		godot::AudioServer *as = godot::AudioServer::get_singleton();
		for (int i = as->get_bus_effect_count(0) - 1; i >= 0; --i) {
			if (as->get_bus_effect(0, i) == capture_) {
				as->remove_bus_effect(0, i);
			}
		}
	}
}

AudioIngameCheck::Snapshot AudioIngameCheck::snapshot(const Bf3GameAudio &p_audio) {
	Snapshot s;
	const ba::MixerCore *mixer = p_audio.mixer();
	if (mixer == nullptr) {
		return s;
	}
	const ba::MixerCutStats &c = mixer->cut_stats();
	const ba::MixerStats &m = mixer->stats();
	const GameAudioStats g = p_audio.stats();
	s.takeover_cuts = c.takeover_cuts.load();
	s.abrupt_gain = c.abrupt_gain.load();
	s.limit_edge_steps = c.limit_edge_steps.load();
	s.overs_out = c.overs_out.load();
	s.held_triggers = c.held_triggers.load();
	s.pan_steps = c.pan_steps.load();
	s.event_drops = c.event_drops.load();
	s.stale_events = c.stale_events.load();
	s.late_blocks = m.late_blocks.load();
	s.late_commands = m.late_commands.load();
	s.ring_overflows = m.ring_overflows.load();
	s.feedback_drops = m.feedback_drops.load();
	s.decode_errors = m.decode_errors.load();
	s.deferred_events = m.deferred_events.load();
	s.pool_stolen = g.one_shots_stolen;
	s.pool_forgotten = g.one_shots_forgotten;
	s.pool_failed = g.one_shots_failed;
	s.router_stolen = g.router.stolen;
	s.router_forgotten = g.router.forgotten;
	s.router_refusals = g.router.mixer_refusals;
	return s;
}

void AudioIngameCheck::mix_row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	if (kind_ == AudioIngamePhase::Mix) {
		row(p_name, p_pass, p_detail);
	} else {
		lines_.push_back("[audio-ingame] " + p_name + " (mix phase only): " + (p_pass ? "pass" : "fail") + " (" +
				p_detail + ")");
	}
}

void AudioIngameCheck::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failures_ += p_pass ? 0u : 1u;
	lines_.push_back("[audio-ingame] " + p_name + ": " + (p_pass ? "PASS" : "FAIL") + " (" + p_detail + ")");
}

void AudioIngameCheck::begin(double p_now_s, Bf3GameAudio &r_audio) {
	godot::AudioServer *as = godot::AudioServer::get_singleton();
	capture_.instantiate();
	capture_->set_buffer_length(2.0f);
	mix_tap_ = std::make_unique<ba::WavRecorder>(ba::kMixRate, seconds_ + 5.0);
	sum_tap_ = std::make_unique<ba::WavRecorder>(ba::kMixRate, seconds_ + 5.0);
	heard_.reserve(size_t((seconds_ + 5.0) * double(ba::kMixRate)) * 2);
	events_.reserve(size_t(1) << 16);
	ledger_.set_categories(&r_audio.sound_categories());
	r_audio.forward_cuts(true);
	r_audio.mixer()->meter(true);
	levels_at_start_ = r_audio.mixer()->levels();
	pool_joins_at_start_ = r_audio.one_shot_join_requests();
	at_start_ = snapshot(r_audio);
	stats_at_start_ = r_audio.stats();
	as->add_bus_effect(0, capture_, -1);
	r_audio.set_mix_tap(mix_tap_.get());
	r_audio.set_sum_tap(sum_tap_.get());
	record_s_ = p_now_s;
}

void AudioIngameCheck::end(Bf3GameAudio &r_audio) {
	r_audio.set_mix_tap(nullptr);
	r_audio.set_sum_tap(nullptr);
	pull();
	godot::AudioServer *as = godot::AudioServer::get_singleton();
	for (int i = as->get_bus_effect_count(0) - 1; i >= 0; --i) {
		if (as->get_bus_effect(0, i) == capture_) {
			as->remove_bus_effect(0, i);
		}
	}
}

void AudioIngameCheck::pull() {
	if (!capture_.is_valid()) {
		return;
	}
	const int32_t n = capture_->get_frames_available();
	if (n > 0) {
		const godot::PackedVector2Array b = capture_->get_buffer(n);
		const int64_t got = b.size();
		const size_t base = heard_.size();
		heard_.resize(base + size_t(got) * 2);
		for (int64_t i = 0; i < got; ++i) {
			heard_[base + size_t(i) * 2] = b[i].x;
			heard_[base + size_t(i) * 2 + 1] = b[i].y;
		}
	}
	discarded_ = capture_->get_discarded_frames();
}

void AudioIngameCheck::take(Bf3GameAudio &r_audio) {
	forwarded_.clear();
	r_audio.take_cuts(forwarded_);
	for (const ba::MixerCutEvent &e : forwarded_) {
		note(e, r_audio);
	}
}

void AudioIngameCheck::note(const ba::MixerCutEvent &p_event, Bf3GameAudio &r_audio) {
	events_.push_back(p_event);
	ledger_.note(p_event, r_audio.scope_name(p_event.scope));
	if (p_event.kind == ba::MixerCutKind::Request) {
		AudioSoundFate f;
		f.program = p_event.program;
		f.request_s = now_s_ - record_s_;
		f.distance_m = p_event.distance_m;
		f.scope = p_event.scope;
		f.category =
				r_audio.sound_categories().category(f.program, r_audio.scope_name(p_event.scope), p_event.distance_m);
		fates_[p_event.handle] = std::move(f);
		return;
	}
	if (p_event.handle == ba::kNoInstance) {
		return;
	}
	if (p_event.kind == ba::MixerCutKind::Refused) {
		refused_.emplace(p_event.handle, now_s_);
	}
	if (p_event.kind == ba::MixerCutKind::Finish || p_event.kind == ba::MixerCutKind::Join ||
			p_event.kind == ba::MixerCutKind::Kill || p_event.kind == ba::MixerCutKind::Cull ||
			p_event.kind == ba::MixerCutKind::Drop) {
		float &peak = sound_peaks_[p_event.handle];
		peak = std::max(peak, p_event.peak);
	}
	if (p_event.kind == ba::MixerCutKind::Join) {
		joins_.emplace_back(p_event.handle, ba::InstanceHandle(p_event.count));
		const auto next = fates_.find(ba::InstanceHandle(p_event.count));
		if (next != fates_.end()) {
			next->second.started = true;
		}
	}
	const auto it = fates_.find(p_event.handle);
	if (it == fates_.end()) {
		++unknown_handles_;
		return;
	}
	AudioSoundFate &f = it->second;
	switch (p_event.kind) {
		case ba::MixerCutKind::Start:
		case ba::MixerCutKind::Resume:
			f.started = true;
			break;
		case ba::MixerCutKind::Finish:
			f.finished = true;
			break;
		case ba::MixerCutKind::Cull:
			f.cull = p_event.reason;
			f.dropped_while_parked = p_event.reason == uint8_t(ba::CullVerdict::Pending);
			break;
		case ba::MixerCutKind::Park:
			f.park = p_event.reason;
			++f.parks;
			break;
		case ba::MixerCutKind::Kill:
			f.killed = true;
			break;
		case ba::MixerCutKind::Refused:
			f.refused = true;
			break;
		case ba::MixerCutKind::Coalesce:
			f.coalesced += p_event.count;
			break;
		case ba::MixerCutKind::Join:
			f.joined = true;
			break;
		default:
			break;
	}
}

std::optional<double> audio_ingame_clip_steps_per_second(
		const anim::VmLocomotion &p_loco, const ba::FootstepParams &p_params, bool p_sprint, float p_speed) {
	const anim::VmDeltaClip &c = p_sprint ? p_loco.sprint : p_loco.run;
	size_t steps = 0;
	for (const anim::VmEventTag &tag : c.tags) {
		steps += tag.frame >= c.start_frame && tag.frame < c.end_frame && ba::animated_step_event(p_params, tag.id)
				? 1u
				: 0u;
	}
	const float len = c.end_frame - c.start_frame;
	const double rate =
			len > 0.0f ? double(steps) * double(anim::vm_loco_fps(c, p_sprint, p_speed)) / double(len) : 0.0;
	return rate > 0.0 && std::isfinite(rate) ? std::optional<double>(rate) : std::nullopt;
}

void AudioIngameCheck::pick_run_yaw(const AudioIngameIo &p_io) {
	if (!p_io.clearance) {
		return;
	}
	float best = 0.0f;
	base_yaw_ = audio_ingame_clearest_yaw(p_io.clearance, base_yaw_, best);
	run_clear_min_m_ = std::min(run_clear_min_m_, best);
	++run_picks_;
}

void AudioIngameCheck::script_steps(double p_now_s, const AudioIngameIo &p_io) {
	if (leg_s_ < 0.0 && !spot_checked_) {
		spot_checked_ = true;
		float clear = 0.0f;
		if (p_io.clearance && p_io.open_spot && p_io.teleport) {
			(void)audio_ingame_clearest_yaw(p_io.clearance, base_yaw_, clear);
			spot_ = clear < kAudioIngameRunOpenMeters ? p_io.open_spot() : std::nullopt;
			spot_s_ = p_now_s;
		}
	}
	if (leg_s_ < 0.0 && spot_) {
		const float off = std::hypot(p_io.feet[0] - spot_->feet[0], p_io.feet[2] - spot_->feet[2]);
		if (off > kAudioIngameArriveMeters && p_now_s - spot_s_ < kAudioIngameArriveSeconds) {
			p_io.teleport(*spot_);
			p_io.move(false, false);
			return;
		}
		teleports_ += off <= kAudioIngameArriveMeters ? 1u : 0u;
		spot_clear_m_ = spot_->clear_m;
		base_yaw_ = spot_->yaw;
		spot_.reset();
	}
	if (leg_s_ < 0.0) {
		leg_s_ = p_now_s;
		leg_ = AudioIngameLeg::Stand;
		leg_index_ = 0;
	}
	if (p_now_s - leg_s_ >= kAudioIngameLegSeconds[size_t(leg_)]) {
		if (record_s_ >= 0.0 && leg_s_ >= record_s_) {
			++legs_[size_t(leg_)].legs;
			stands_after_moving_ += leg_ == AudioIngameLeg::Stand && leg_index_ != 0 ? 1u : 0u;
		}
		++leg_index_;
		leg_ = leg_index_ % 2 == 0 ? AudioIngameLeg::Stand
								   : (leg_index_ % 4 == 1 ? AudioIngameLeg::Walk : AudioIngameLeg::Sprint);
		leg_s_ = p_now_s;
		last_step_s_ = -1.0;
		if (leg_ != AudioIngameLeg::Stand) {
			pick_run_yaw(p_io);
		}
	}
	p_io.view(base_yaw_, 0.0f);
	p_io.move(leg_ != AudioIngameLeg::Stand, leg_ == AudioIngameLeg::Sprint);
}

void AudioIngameCheck::tally_steps(double p_now_s, Bf3GameAudio &r_audio, const AudioIngameIo &p_io) {
	const bool settled = p_io.alive && leg_s_ >= 0.0 && p_now_s - leg_s_ >= kAudioIngameLegSettleSeconds;
	AudioIngameLegTally &t = legs_[size_t(leg_)];
	if (settled && tally_s_ >= 0.0) {
		t.seconds += p_now_s - tally_s_;
	}
	tally_s_ = p_now_s;
	if (settled && last_feet_) {
		t.moved_m += std::hypot(p_io.feet[0] - (*last_feet_)[0], p_io.feet[2] - (*last_feet_)[2]);
	}
	last_feet_ = p_io.alive ? std::optional<sim::Vec3>(p_io.feet) : std::nullopt;
	if (!step_params_) {
		step_params_ = r_audio.footstep_params();
	}
	for (const ba::FootstepPost &post : r_audio.footstep_posts()) {
		if (post.slot != p_io.own_slot) {
			++other_steps_;
			continue;
		}
		if (post.event == ba::FootstepEvent::Start) {
			++t.starts;
		} else if (post.event == ba::FootstepEvent::Stop) {
			++t.stops;
		} else if (ba::footstep_plant(post.event) && settled) {
			t.animated += post.event == ba::FootstepEvent::Animated ? 1u : 0u;
			t.speed_min = t.steps == 0 ? post.speed : std::min(t.speed_min, post.speed);
			t.speed_max = t.steps == 0 ? post.speed : std::max(t.speed_max, post.speed);
			t.speed_sum += double(post.speed);
			++t.steps;
			if (last_step_s_ >= 0.0) {
				t.intervals.emplace_back(post.speed, p_now_s - last_step_s_);
				const std::optional<anim::VmLocomotion> loco = p_io.locomotion ? p_io.locomotion() : std::nullopt;
				const std::optional<double> rate = loco && step_params_
						? audio_ingame_clip_steps_per_second(
								  *loco, *step_params_, leg_ == AudioIngameLeg::Sprint, post.speed)
						: std::nullopt;
				t.clip_intervals.push_back(rate ? 1.0 / *rate : 0.0);
			}
			last_step_s_ = p_now_s;
		}
	}
}

void AudioIngameCheck::script(double p_now_s, const AudioIngameIo &p_io) {
	if (!p_io.alive) {
		if (firing_) {
			p_io.hold_fire(false);
			firing_ = false;
		}
		if (kind_ == AudioIngamePhase::Pool && !primary_picked_ && p_io.pick_primary) {
			primary_name_ = p_io.pick_primary(step_patches_);
			primary_picked_ = !primary_name_.empty();
		}
		deaths_ += was_alive_ ? 1u : 0u;
		was_alive_ = false;
		if (p_io.move) {
			p_io.move(false, false);
		}
		leg_s_ = -1.0;
		spot_checked_ = false;
		fight_.reset();
		if (p_io.time_to_respawn <= 0.0f && p_now_s - last_deploy_s_ > 1.5) {
			p_io.deploy();
			last_deploy_s_ = p_now_s;
		}
		return;
	}
	if (!was_alive_) {
		was_alive_ = true;
		alive_s_ = p_now_s;
		have_view_ = false;
		last_step_ = UINT32_MAX;
	}
	const double in = p_now_s - alive_s_;
	if (kind_ == AudioIngamePhase::Flyby && fights_ == 0 && in < kAudioIngameSettleSeconds && p_io.fight_spot &&
			p_io.teleport) {
		fight_ = p_io.fight_spot();
		if (fight_) {
			p_io.teleport(*fight_);
			base_yaw_ = fight_->yaw;
			++fights_;
			fight_after_s_ = std::max(fight_after_s_, in);
		}
	}
	if (in < kAudioIngameSettleSeconds) {
		base_yaw_ = fight_ ? fight_->yaw : p_io.yaw();
		last_s_ = p_now_s;
		return;
	}
	if (kind_ == AudioIngamePhase::Steps) {
		script_steps(p_now_s, p_io);
		return;
	}
	if (kind_ == AudioIngamePhase::Flyby) {
		if (!have_view_) {
			have_view_ = true;
			yaw_ = p_io.yaw();
		}
		const float max_step =
				float(std::clamp(p_now_s - last_s_, 0.0, 0.1)) * kAudioIngameTurnDegPerSecond * core::kDegToRadF;
		yaw_ += std::clamp(std::remainder(base_yaw_ - yaw_, 2.0f * std::numbers::pi_v<float>), -max_step, max_step);
		last_s_ = p_now_s;
		p_io.view(yaw_, 0.0f);
		return;
	}
	double t = in - kAudioIngameSettleSeconds;
	uint32_t step = 0;
	for (;;) {
		const double len =
				kAudioIngameHoldSeconds + (step % 2 == 1 ? kAudioIngameReloadPauseSeconds : kAudioIngamePauseSeconds);
		if (t < len) {
			break;
		}
		t -= len;
		++step;
	}
	const AudioIngameAim &a = kAudioIngameAims[step % kAudioIngameAims.size()];
	const float want_yaw = base_yaw_ + a.yaw_deg * core::kDegToRadF, want_pitch = a.pitch_deg * core::kDegToRadF;
	if (!have_view_) {
		have_view_ = true;
		yaw_ = want_yaw;
		pitch_ = want_pitch;
	}
	const float max_step =
			float(std::clamp(p_now_s - last_s_, 0.0, 0.1)) * kAudioIngameTurnDegPerSecond * core::kDegToRadF;
	yaw_ += std::clamp(want_yaw - yaw_, -max_step, max_step);
	pitch_ += std::clamp(want_pitch - pitch_, -max_step, max_step);
	last_s_ = p_now_s;
	p_io.view(yaw_, pitch_);
	const bool pool = kind_ == AudioIngamePhase::Pool;
	if (pool && step != last_step_) {
		last_step_ = step;
		if (step % 3 == 2) {
			p_io.grenade();
			++grenades_;
		}
	}
	const bool want = t < kAudioIngameHoldSeconds && !(pool && step % 3 == 2);
	if (!want && firing_ && step % 2 == 1) {
		p_io.hold_fire(false);
		firing_ = false;
		p_io.reload();
	}
	if (want != firing_) {
		p_io.hold_fire(want);
		firing_ = want;
		if (record_s_ >= 0.0 && want) {
			own_fire_.emplace_back(p_now_s - record_s_, -1.0);
		} else if (record_s_ >= 0.0 && !own_fire_.empty() && own_fire_.back().second < 0.0) {
			own_fire_.back().second = p_now_s - record_s_;
		}
		bursts_ += want ? 1u : 0u;
	}
}

bool AudioIngameCheck::frame(double p_now_s, Bf3GameAudio &r_audio, const AudioIngameIo &p_io) {
	if (done_) {
		return true;
	}
	if (!r_audio.built() || r_audio.mixer() == nullptr) {
		return false;
	}
	now_s_ = p_now_s;
	if (step_patches_.empty()) {
		step_patches_ = r_audio.weapon_patches(false, audio::kHashStep);
	}
	if (first_s_ < 0.0) {
		first_s_ = p_now_s;
	}
	switch (phase_) {
		case Phase::WaitSpawn:
			script(p_now_s, p_io);
			if (p_io.alive) {
				spawn_s_ = p_now_s;
				phase_ = Phase::Settle;
			} else if (p_now_s - first_s_ > kAudioIngameSpawnTimeoutSeconds) {
				row("spawn", false,
						core::fmt(
								"no spawn within %.0f s of the game audio starting", kAudioIngameSpawnTimeoutSeconds));
				done_ = true;
				return true;
			}
			return false;
		case Phase::Settle:
			script(p_now_s, p_io);
			if (p_now_s - spawn_s_ >= kAudioIngameSettleSeconds) {
				begin(p_now_s, r_audio);
				phase_ = Phase::Record;
			}
			return false;
		case Phase::Record:
			script(p_now_s, p_io);
			if (kind_ == AudioIngamePhase::Steps) {
				tally_steps(p_now_s, r_audio, p_io);
			}
			pull();
			take(r_audio);
			if (p_now_s - record_s_ < seconds_) {
				return false;
			}
			if (firing_) {
				p_io.hold_fire(false);
				firing_ = false;
			}
			if (p_io.move) {
				p_io.move(false, false);
			}
			end(r_audio);
			r_audio.drain_events();
			take(r_audio);
			r_audio.forward_cuts(false);
			analyse(r_audio);
			phase_ = Phase::Done;
			done_ = true;
			return true;
		case Phase::Done:
			return true;
	}
	return true;
}

void AudioIngameCheck::analyse(Bf3GameAudio &r_audio) {
	const std::span<const float> mix = mix_tap_->samples();
	const std::span<const float> heard(heard_);
	const size_t hf = heard.size() / 2, mf = mix.size() / 2;
	size_t g0 = SIZE_MAX;
	for (size_t i = kAudioIngameAlignSearchFrames; i < hf; ++i) {
		if (std::fabs(heard[i * 2]) > kAudioIngameSoundLevel || std::fabs(heard[i * 2 + 1]) > kAudioIngameSoundLevel) {
			g0 = i;
			break;
		}
	}
	int64_t offset = 0;
	bool aligned = false;
	if (g0 != SIZE_MAX && g0 + kAudioIngameAlignWindowFrames <= hf) {
		const int64_t search = kAudioIngameAlignSearchFrames;
		for (int64_t o = -search; o <= search && !aligned; ++o) {
			const int64_t m0 = int64_t(g0) + o;
			if (m0 < 0 || size_t(m0) + kAudioIngameAlignWindowFrames > mf) {
				continue;
			}
			bool same = true;
			for (size_t i = 0; i < size_t(kAudioIngameAlignWindowFrames) * 2 && same; ++i) {
				same = std::fabs(heard[g0 * 2 + i] - mix[size_t(m0) * 2 + i]) <= kAudioIngameSameSample;
			}
			if (same) {
				aligned = true;
				offset = o;
			}
		}
	}
	uint64_t mismatched = 0, overlap = 0;
	if (aligned) {
		for (size_t i = 0; i < hf; ++i) {
			const int64_t m = int64_t(i) + offset;
			if (m < 0 || size_t(m) >= mf) {
				continue;
			}
			++overlap;
			mismatched += std::fabs(heard[i * 2] - mix[size_t(m) * 2]) > kAudioIngameSameSample ||
							std::fabs(heard[i * 2 + 1] - mix[size_t(m) * 2 + 1]) > kAudioIngameSameSample
					? 1u
					: 0u;
		}
	}
	const uint64_t expected = uint64_t(seconds_ * double(ba::kMixRate));
	row("device path", aligned && mismatched == 0 && discarded_ == 0 && overlap * 10 >= expected * 9,
			core::fmt("Godot's Master bus capture %zu frames, mixer output %zu frames, offset %lld, %llu of %llu "
					  "frames differ, %lld discarded",
					hf, mf, (long long)offset, (unsigned long long)mismatched, (unsigned long long)overlap,
					(long long)discarded_));

	uint64_t overs = 0;
	float peak = 0.0f;
	for (const float x : heard) {
		peak = std::max(peak, std::fabs(x));
		overs += std::fabs(x) > 1.0f || !std::isfinite(x) ? 1u : 0u;
	}
	const Snapshot now = snapshot(r_audio);
	const auto delta = [&](uint64_t Snapshot::*p_field) { return now.*p_field - at_start_.*p_field; };
	row("no clipping", overs == 0 && delta(&Snapshot::overs_out) == 0,
			core::fmt("peak %.4f, %llu samples over 0 dBFS at the device, %llu after the limiter", double(peak),
					(unsigned long long)overs, (unsigned long long)delta(&Snapshot::overs_out)));

	uint64_t zero_runs = 0;
	size_t run = 0;
	bool sounding = false;
	for (size_t i = 0; i < hf; ++i) {
		const float a = std::max(std::fabs(heard[i * 2]), std::fabs(heard[i * 2 + 1]));
		if (a == 0.0f) {
			++run;
			continue;
		}
		zero_runs += sounding && run >= kAudioIngameZeroRunFrames && a > kAudioIngameSoundLevel ? 1u : 0u;
		run = 0;
		sounding = a > kAudioIngameSoundLevel;
	}
	uint64_t call_gaps = 0;
	float call_gap_max = 0.0f;
	std::map<std::string, uint64_t> audible;
	const ba::MixerCutEvent *largest = nullptr;
	for (const ba::MixerCutEvent &e : events_) {
		if (e.kind == ba::MixerCutKind::CallGap) {
			++call_gaps;
			call_gap_max = std::max(call_gap_max, e.step);
		}
		if (!silent_kind(e.kind) && e.step > ba::kAudibleStep) {
			++audible[std::string(ba::mixer_cut_kind_name(e.kind)) + " " + e.program];
			largest = largest == nullptr || e.step > largest->step ? &e : largest;
		}
	}
	row("no dropout", zero_runs == 0 && delta(&Snapshot::late_blocks) == 0 && call_gaps == 0,
			core::fmt("%llu silent runs of %u+ frames inside sound, %llu late blocks, %llu mix() calls later than %.0f "
					  "ms (max %.1f ms)",
					(unsigned long long)zero_runs, kAudioIngameZeroRunFrames,
					(unsigned long long)delta(&Snapshot::late_blocks), (unsigned long long)call_gaps, ba::kLateCallMs,
					double(call_gap_max)));
	uint64_t audible_total = 0;
	std::string audible_text;
	for (const auto &[what, n] : audible) {
		audible_total += n;
		if (audible_text.size() < 300) {
			audible_text += "; " + what + " x" + std::to_string(n);
		}
	}
	if (largest != nullptr) {
		audible_text += core::fmt("; the largest a %s of handle %u at %.1f m, step %.5f, peak %.5f",
				ba::mixer_cut_kind_name(largest->kind), unsigned(largest->handle), double(largest->distance_m),
				double(largest->step), double(largest->peak));
	}
	row("no hard cut",
			audible_total == 0 && delta(&Snapshot::takeover_cuts) == 0 && delta(&Snapshot::abrupt_gain) == 0 &&
					delta(&Snapshot::limit_edge_steps) == 0 && delta(&Snapshot::pan_steps) == 0,
			core::fmt("%llu audible cuts, %llu takeover cuts, %llu abrupt gain steps, %llu limiter edge steps, %llu "
					  "pan gain steps",
					(unsigned long long)audible_total, (unsigned long long)delta(&Snapshot::takeover_cuts),
					(unsigned long long)delta(&Snapshot::abrupt_gain),
					(unsigned long long)delta(&Snapshot::limit_edge_steps),
					(unsigned long long)delta(&Snapshot::pan_steps)) +
					audible_text);
	row("no retrigger", delta(&Snapshot::held_triggers) == 0,
			core::fmt("%llu sampler triggers from a trigger input still on since the previous evaluation",
					(unsigned long long)delta(&Snapshot::held_triggers)));
	mix_row("no sound outlives its data",
			delta(&Snapshot::pool_forgotten) == 0 && delta(&Snapshot::router_forgotten) == 0,
			core::fmt("%llu one-shots and %llu weapon voices killed for never finishing",
					(unsigned long long)delta(&Snapshot::pool_forgotten),
					(unsigned long long)delta(&Snapshot::router_forgotten)));

	std::array<AudioCategoryCounts, size_t(ba::SoundCategory::Count)> cats;
	std::string fates_csv = "handle,category,request_s,distance_m,scope,started,finished,cull,park,parks,refused,"
							"killed,joined,coalesced,program\n";
	for (const auto &[h, f] : fates_) {
		AudioCategoryCounts &c = cats[size_t(f.category)];
		++c.requested;
		c.started += f.started ? 1u : 0u;
		c.finished += f.finished ? 1u : 0u;
		c.joined += f.joined ? 1u : 0u;
		if (!f.started && f.cull == 0 && f.parks == 0 && !f.refused && !f.killed && !f.joined) {
			++c.other_drops;
			++c.reasons["requested, never started or dropped"];
		}
		const std::string scope(r_audio.scope_name(f.scope));
		if (f.cull != 0 && !f.dropped_while_parked) {
			(data_reason(f.cull) ? c.data_drops : c.other_drops) += 1;
			++c.reasons["dropped: " + reason_text(f.cull, scope)];
		} else if (f.dropped_while_parked) {
			(data_reason(f.park) ? c.data_drops : c.other_drops) += 1;
			++c.reasons["stopped while parked: " + reason_text(f.park, scope)];
		} else if (f.parks > 0 && f.park == uint8_t(ba::CullVerdict::Cap)) {
			++c.other_drops;
			++c.reasons["parked mid-sound: voice cap"];
		} else if (f.parks > 0) {
			++c.data_drops;
			++c.reasons["parked mid-sound: " + reason_text(f.park, scope)];
		}
		if (f.refused) {
			++c.other_drops;
			++c.reasons["refused: no mixer slot"];
		}
		if (f.killed) {
			++c.other_drops;
			++c.reasons["killed by the pool or the router"];
		}
		if (f.coalesced > 0) {
			++c.other_drops;
			c.reasons["events dropped at a resume"] += f.coalesced;
		}
		fates_csv += core::fmt("%u,%s,%.3f,%.2f,%s,%d,%d,%u,%u,%u,%d,%d,%d,%u,", unsigned(h),
				std::string(ba::kSoundCategoryNames[size_t(f.category)]).c_str(), f.request_s, double(f.distance_m),
				scope.c_str(), int(f.started), int(f.finished), unsigned(f.cull), unsigned(f.park), f.parks,
				int(f.refused), int(f.killed), int(f.joined), f.coalesced);
		fates_csv += f.program + "\n";
	}
	uint64_t other_drops = 0;
	for (size_t k = 0; k < cats.size(); ++k) {
		const AudioCategoryCounts &c = cats[k];
		other_drops += c.other_drops;
		std::string reasons;
		for (const auto &[why, n] : c.reasons) {
			reasons += ", " + why + " " + std::to_string(n);
		}
		lines_.push_back(
				core::fmt("[audio-ingame] sounds %s: %llu requested, %llu started, %llu finished, %llu joined "
						  "the next burst's instance, %llu dropped by the data's rules, %llu dropped otherwise",
						std::string(ba::kSoundCategoryNames[k]).c_str(), (unsigned long long)c.requested,
						(unsigned long long)c.started, (unsigned long long)c.finished, (unsigned long long)c.joined,
						(unsigned long long)c.data_drops, (unsigned long long)c.other_drops) +
				reasons);
	}
	const uint64_t lost_counters = delta(&Snapshot::pool_stolen) + delta(&Snapshot::router_stolen) +
			delta(&Snapshot::event_drops) + delta(&Snapshot::ring_overflows) + delta(&Snapshot::feedback_drops) +
			delta(&Snapshot::router_refusals) + delta(&Snapshot::pool_failed);
	mix_row("drops only by the data's rules", other_drops == 0 && lost_counters == 0,
			core::fmt("%llu sounds dropped outside the data's rules; pool steals %llu, router steals %llu, refusals "
					  "%llu, pool failures %llu, cut events lost %llu, "
					  "command ring overflows %llu, feedback drops %llu, stale events at a resume %llu, %llu events of "
					  "sounds requested before the window",
					(unsigned long long)other_drops, (unsigned long long)delta(&Snapshot::pool_stolen),
					(unsigned long long)delta(&Snapshot::router_stolen),
					(unsigned long long)delta(&Snapshot::router_refusals),
					(unsigned long long)delta(&Snapshot::pool_failed),
					(unsigned long long)delta(&Snapshot::event_drops),
					(unsigned long long)delta(&Snapshot::ring_overflows),
					(unsigned long long)delta(&Snapshot::feedback_drops),
					(unsigned long long)delta(&Snapshot::stale_events), (unsigned long long)unknown_handles_));
	const ba::SoundFateTotals ledger = ledger_.totals();
	bool ledger_same = ledger_.overflows() == 0;
	std::string ledger_text;
	for (size_t k = 0; k < cats.size(); ++k) {
		const ba::SoundFateCounts &l = ledger.categories[k];
		const AudioCategoryCounts &c = cats[k];
		ledger_same = ledger_same && l.requested == c.requested && l.data_drops == c.data_drops &&
				l.other_drops == c.other_drops && l.started == c.started && l.joined == c.joined;
		ledger_text += core::fmt("; %s %llu/%llu/%llu requested/data/other (check %llu/%llu/%llu)",
				std::string(ba::kSoundCategoryNames[k]).c_str(), (unsigned long long)l.requested,
				(unsigned long long)l.data_drops, (unsigned long long)l.other_drops, (unsigned long long)c.requested,
				(unsigned long long)c.data_drops, (unsigned long long)c.other_drops);
	}
	row("the in-game ledger counts what the check counts", ledger_same,
			core::fmt("SoundFateLedger %llu outside / %llu by the data's rules, %llu over capacity",
					(unsigned long long)ledger.other_drops(), (unsigned long long)ledger.data_drops(),
					(unsigned long long)ledger_.overflows()) +
					ledger_text);
	const AudioCategoryCounts &own = cats[size_t(ba::SoundCategory::OwnGun)];
	const AudioCategoryCounts &bots = cats[size_t(ba::SoundCategory::BotGun)];
	const AudioCategoryCounts &surface = cats[size_t(ba::SoundCategory::ImpactSurface)];
	const bool moved = legs_[size_t(AudioIngameLeg::Walk)].legs >= kAudioIngameMinLegs &&
			legs_[size_t(AudioIngameLeg::Sprint)].legs >= kAudioIngameMinLegs;
	const bool fired = kind_ == AudioIngamePhase::Pool || bursts_ >= 6;
	const bool ran = kind_ == AudioIngamePhase::Steps ? moved
			: kind_ == AudioIngamePhase::Flyby
			? bursts_ == 0 && bots.requested >= kAudioIngameMinBotShots && surface.requested >= 20
			: fired && own.requested >= 1 && bots.requested >= 10 && surface.requested >= 20;
	row("the scenario ran", ran,
			core::fmt("%.1f s recorded, %u own bursts, %u own deaths, %llu own gun, %llu bot gun and %llu surface "
					  "impact sounds requested",
					seconds_, bursts_, deaths_, (unsigned long long)own.requested, (unsigned long long)bots.requested,
					(unsigned long long)surface.requested));
	analyse_mix(r_audio);
	if (kind_ == AudioIngamePhase::Pool) {
		analyse_pool(r_audio);
	}
	if (kind_ == AudioIngamePhase::Steps) {
		analyse_steps(r_audio);
	}
	if (kind_ == AudioIngamePhase::Flyby) {
		analyse_flyby(r_audio);
	}

	if (!out_dir_.empty()) {
		const core::PathGuard &guard = r_audio.write_guard();
		const std::filesystem::path dir = utf8_path(out_dir_);
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const core::Status w1 =
				ba::write_wav(guard, dir / "godot_out.wav", heard, 2, ba::kMixRate, ba::WavSampleFormat::Pcm16);
		const core::Status w2 = ba::write_wav(guard, dir / "mix.wav", mix, 2, ba::kMixRate, ba::WavSampleFormat::Pcm16);
		bool csv_ok = guard.allows_write(dir / "fates.csv");
		if (csv_ok) {
			std::ofstream f(dir / "fates.csv", std::ios::binary | std::ios::trunc);
			f << fates_csv;
			csv_ok = f.good();
		}
		row("files", w1.ok() && w2.ok() && csv_ok,
				"godot_out.wav, mix.wav and fates.csv in " + out_dir_ + (w1.ok() ? "" : ": " + w1.error().message) +
						(w2.ok() ? "" : ": " + w2.error().message));
	}
}

void AudioIngameCheck::analyse_mix(Bf3GameAudio &r_audio) {
	const ba::MixerLevels l = r_audio.mixer()->levels();
	const uint64_t blocks = l.blocks - levels_at_start_.blocks;
	const double samples = double(std::max<uint64_t>(blocks, 1)) * double(ba::kBlockFrames) * 2.0;
	const auto peak_db = [](float p_peak) { return p_peak > 0.0f ? 20.0 * std::log10(double(p_peak)) : -200.0; };
	const auto rms_db = [&](double p_energy) {
		return p_energy > 0.0 ? 10.0 * std::log10(p_energy / samples) : -200.0;
	};
	std::string buses;
	for (size_t b = 0; b < ba::kMeteredBuses; ++b) {
		const double e = l.bus_energy[b] - levels_at_start_.bus_energy[b];
		if (l.bus_peak[b] > 0.0f) {
			buses += core::fmt("; %s peak %.1f dBFS, RMS %.1f dBFS",
					std::string(core::audio::master_bus_name(core::audio::MasterBus(b))).c_str(),
					peak_db(l.bus_peak[b]), rms_db(e));
		}
	}
	lines_.push_back(core::fmt("[audio-ingame] mix: %llu blocks; before the limiter peak %.1f dBFS, RMS %.1f dBFS",
							 (unsigned long long)blocks, peak_db(l.pre_limit_peak),
							 rms_db(l.pre_limit_energy - levels_at_start_.pre_limit_energy)) +
			buses);
	std::array<uint64_t, ba::kLimitEdgesDb.size() + 1> limit{};
	uint64_t limited = 0, below_floor = 0;
	std::string hist;
	for (size_t k = 0; k < limit.size(); ++k) {
		limit[k] = l.limit_blocks[k] - levels_at_start_.limit_blocks[k];
		limited += k > 0 ? limit[k] : 0u;
		below_floor += k > 0 && ba::kLimitEdgesDb[k - 1] <= kAudioIngameLimitFloorDb ? limit[k] : 0u;
		hist += k == 0
				? core::fmt(" above %.1f dB %llu", double(ba::kLimitEdgesDb[0]), (unsigned long long)limit[k])
				: core::fmt(", below %.1f dB %llu", double(ba::kLimitEdgesDb[k - 1]), (unsigned long long)limit[k]);
	}
	const bool limit_ok = below_floor == 0 &&
			double(limited) <= kAudioIngameLimitedBlocksShare * double(std::max<uint64_t>(blocks, 1));
	const std::string limit_text =
			core::fmt("blocks by the limiter's lowest gain:%s; %llu of %llu blocks limited, %llu "
					  "below %.1f dB",
					hist.c_str(), (unsigned long long)limited, (unsigned long long)blocks,
					(unsigned long long)below_floor, double(kAudioIngameLimitFloorDb));
	if (kind_ == AudioIngamePhase::Mix) {
		row("limiter engagement", limit_ok, limit_text);
	} else {
		lines_.push_back("[audio-ingame] limiter: " + limit_text);
	}
	const uint64_t samples_out = blocks * ba::kBlockFrames * 2;
	const std::string master_text = l.master_preset < 0
			? std::string("no master mix (the buses are summed)")
			: core::fmt("master preset %d at %u speakers: its compressor down to %.1f dB, %llu of %llu samples cut by "
						"its output clip",
					  l.master_preset, l.master_speakers, double(l.master_compressor_min_db),
					  (unsigned long long)l.master_ceiling_clipped_samples, (unsigned long long)samples_out);
	mix_row("the master mix",
			l.master_preset >= 0 &&
					double(l.master_ceiling_clipped_samples) <=
							kAudioIngameLimitedBlocksShare * double(std::max<uint64_t>(samples_out, 1)),
			master_text);
	struct Balance {
		double own_db = -200.0, rest_db = -200.0, own_rms_db = -200.0, rest_rms_db = -200.0;
		size_t bursts = 0, windows = 0;
	};
	using Span = std::pair<double, double>;
	const auto level_in = [](std::span<const float> p_x, const Span &p_span) {
		const double seconds = double(p_x.size() / 2) / double(ba::kMixRate);
		const size_t a = size_t(std::clamp(p_span.first, 0.0, seconds) * double(ba::kMixRate));
		const size_t b = size_t(std::clamp(p_span.second, 0.0, seconds) * double(ba::kMixRate));
		float p = 0.0f;
		double e = 0.0;
		for (size_t i = a * 2; i < b * 2; ++i) {
			p = std::max(p, std::fabs(p_x[i]));
			e += double(p_x[i]) * double(p_x[i]);
		}
		return std::pair<float, double>{ p,
			b > a ? 10.0 * std::log10(std::max(e / double((b - a) * 2), 1.0e-30)) : -200.0 };
	};
	const std::span<const float> sum = sum_tap_->samples();
	std::vector<Span> fire, own_spans, rest_spans;
	for (const auto &[s, e] : own_fire_) {
		fire.emplace_back(s, (e < 0.0 ? s + kAudioIngameHoldSeconds : e) + kAudioIngameOwnTailSeconds);
	}
	for (const Span &f : fire) {
		if (level_in(sum, f).first > kAudioIngameRestFloor) {
			own_spans.push_back(f);
		}
	}
	const double sum_seconds = double(sum.size() / 2) / double(ba::kMixRate);
	for (double w = 0.0; w + kAudioIngameRestWindowSeconds <= sum_seconds; w += kAudioIngameRestWindowSeconds) {
		const bool near_own = std::any_of(fire.begin(), fire.end(), [&](const Span &f) {
			return w + kAudioIngameRestWindowSeconds > f.first - kAudioIngameOwnGuardSeconds &&
					w < f.second + kAudioIngameOwnGuardSeconds;
		});
		const Span span{ w, w + kAudioIngameRestWindowSeconds };
		if (!near_own && level_in(sum, span).first > kAudioIngameRestFloor) {
			rest_spans.push_back(span);
		}
	}
	const auto balance = [&](std::span<const float> p_x) {
		std::vector<double> own, rest, own_rms, rest_rms;
		for (const Span &s : own_spans) {
			const auto [p, rms] = level_in(p_x, s);
			own.push_back(p > 0.0f ? 20.0 * std::log10(double(p)) : -200.0);
			own_rms.push_back(rms);
		}
		for (const Span &s : rest_spans) {
			const auto [p, rms] = level_in(p_x, s);
			rest.push_back(p > 0.0f ? 20.0 * std::log10(double(p)) : -200.0);
			rest_rms.push_back(rms);
		}
		Balance b;
		b.bursts = own.size();
		b.windows = rest.size();
		if (!own.empty() && !rest.empty()) {
			for (std::vector<double> *v : { &own, &rest, &own_rms, &rest_rms }) {
				std::sort(v->begin(), v->end());
			}
			b.own_db = own[own.size() / 2];
			b.rest_db = rest[rest.size() / 2];
			b.own_rms_db = own_rms[own_rms.size() / 2];
			b.rest_rms_db = rest_rms[rest_rms.size() / 2];
		}
		return b;
	};
	const Balance before = balance(sum);
	const Balance after = balance(mix_tap_->samples());
	const double shift = (after.own_rms_db - after.rest_rms_db) - (before.own_rms_db - before.rest_rms_db);
	const double peak_shift = (after.own_db - after.rest_db) - (before.own_db - before.rest_db);
	const std::string balance_text =
			core::fmt("own bursts against the mix away from own fire (medians of %zu bursts and %zu %.2f s windows): "
					  "RMS %.1f dB in the "
					  "bus sum before the master (%.1f / %.1f dBFS), %.1f dB at the output (%.1f / %.1f dBFS), a shift "
					  "of %+.1f dB; "
					  "peak %.1f dB before (%.1f / %.1f dBFS), %.1f dB after (%.1f / %.1f dBFS), a shift of %+.1f dB",
					after.bursts, after.windows, kAudioIngameRestWindowSeconds, before.own_rms_db - before.rest_rms_db,
					before.own_rms_db, before.rest_rms_db, after.own_rms_db - after.rest_rms_db, after.own_rms_db,
					after.rest_rms_db, shift, before.own_db - before.rest_db, before.own_db, before.rest_db,
					after.own_db - after.rest_db, after.own_db, after.rest_db, peak_shift);
	mix_row("own shot level against the rest",
			before.bursts >= 3 && before.windows >= 10 && after.bursts >= 3 && after.windows >= 10 &&
					std::fabs(shift) <= kAudioIngameBalanceDb,
			balance_text);
	const double lufs = ba::integrated_loudness_lufs(mix_tap_->samples());
	std::array<std::vector<double>, size_t(ba::SoundCategory::Count)> peaks;
	for (const auto &[h, p] : sound_peaks_) {
		const auto f = fates_.find(h);
		if (f != fates_.end() && p > 0.0f) {
			peaks[size_t(f->second.category)].push_back(peak_db(p));
		}
	}
	std::string cats;
	std::array<double, size_t(ba::SoundCategory::Count)> medians{};
	for (size_t k = 0; k < peaks.size(); ++k) {
		std::vector<double> &v = peaks[k];
		medians[k] = -200.0;
		if (v.empty()) {
			continue;
		}
		std::sort(v.begin(), v.end());
		medians[k] = v[v.size() / 2];
		cats += core::fmt("; %s %zu sounds, median peak %.1f dBFS, loudest %.1f dBFS",
				std::string(ba::kSoundCategoryNames[k]).c_str(), v.size(), medians[k], v.back());
	}
	lines_.push_back(core::fmt("[audio-ingame] loudness: Master integrated %.1f LUFS over %.1f s; own gun median peak "
							   "%.1f dB relative to bot guns' (before the master)",
							 lufs, seconds_,
							 medians[size_t(ba::SoundCategory::OwnGun)] - medians[size_t(ba::SoundCategory::BotGun)]) +
			cats);
}

void AudioIngameCheck::analyse_pool(Bf3GameAudio &r_audio) {
	uint64_t ghosts = 0;
	for (const auto &[h, at] : refused_) {
		ghosts += now_s_ - at > kAudioIngameGhostSeconds && r_audio.one_shot_tracked(h) ? 1u : 0u;
	}
	row("refusals happened", !refused_.empty(),
			core::fmt("%zu starts refused for want of an instance slot (run with a small --audio-max-instances)",
					refused_.size()));
	row("no ghost one-shots", ghosts == 0,
			core::fmt("%llu refused one-shots still held by the OneShotPool %.0f s after the refusal",
					(unsigned long long)ghosts, kAudioIngameGhostSeconds));
	const std::vector<std::string> explosive = r_audio.weapon_patches(true, 0);
	uint64_t shots = 0, throws = 0, throws_explosion = 0;
	for (const auto &[h, f] : fates_) {
		if (f.category == ba::SoundCategory::OwnGun &&
				std::find(step_patches_.begin(), step_patches_.end(), f.program) != step_patches_.end()) {
			++shots;
		}
		if (std::find(explosive.begin(), explosive.end(), f.program) != explosive.end()) {
			++throws;
			throws_explosion += f.category == ba::SoundCategory::Explosion ? 1u : 0u;
		}
	}
	const auto own_step = [&](ba::InstanceHandle p_h) {
		const auto it = fates_.find(p_h);
		return it != fates_.end() && it->second.category == ba::SoundCategory::OwnGun &&
						std::find(step_patches_.begin(), step_patches_.end(), it->second.program) != step_patches_.end()
				? &it->second
				: nullptr;
	};
	uint64_t consecutive = 0, same_frame = 0;
	for (const auto &[old_h, new_h] : joins_) {
		const AudioSoundFate *a = own_step(old_h);
		const AudioSoundFate *b = own_step(new_h);
		if (a != nullptr && b != nullptr) {
			(std::fabs(b->request_s - a->request_s) > kAudioIngameSameShotSeconds ? consecutive : same_frame) += 1;
		}
	}
	const uint64_t join_requests = r_audio.one_shot_join_requests() - pool_joins_at_start_;
	const GameAudioStats g = r_audio.stats();
	row("one-shots join", primary_picked_ && shots >= 6 && consecutive >= 2,
			core::fmt("own primary %s (%zu Step patches): %llu own Step one-shots requested, %llu joined by a later "
					  "shot, "
					  "%llu by a request of the same frame; %llu pool join requests; router %llu one-shots and %llu "
					  "foley "
					  "one-shots in the session",
					primary_picked_ ? primary_name_.c_str() : "NOT picked", step_patches_.size(),
					(unsigned long long)shots, (unsigned long long)consecutive, (unsigned long long)same_frame,
					(unsigned long long)join_requests, (unsigned long long)g.router.one_shots,
					(unsigned long long)g.router.foley));
	row("the M67 counts as an explosion", throws >= 1 && throws_explosion == throws,
			core::fmt("%u grenade presses: %llu throw sounds requested, %llu filed as explosion", grenades_,
					(unsigned long long)throws, (unsigned long long)throws_explosion));
}

void AudioIngameCheck::analyse_steps(Bf3GameAudio &r_audio) {
	const auto cadence = [](const AudioIngameLegTally &p_t) {
		return p_t.seconds > 0.0 ? double(p_t.steps) / p_t.seconds : 0.0;
	};
	const auto mean = [](const AudioIngameLegTally &p_t) {
		return p_t.steps > 0 ? p_t.speed_sum / double(p_t.steps) : 0.0;
	};
	const auto tally_text = [&](AudioIngameLeg p_leg) {
		const AudioIngameLegTally &t = legs_[size_t(p_leg)];
		return core::fmt("%s: %u legs, %llu own steps (%llu from the 1p animation) over %.1f s (%.2f steps/s), speed "
						 "%.2f..%.2f mean %.2f m/s, the own feet moved %.1f m (%.2f m/s), %llu starts, %llu stops",
				std::string(kAudioIngameLegNames[size_t(p_leg)]).c_str(), t.legs, (unsigned long long)t.steps,
				(unsigned long long)t.animated, t.seconds, cadence(t), double(t.speed_min), double(t.speed_max),
				mean(t), t.moved_m, t.seconds > 0.0 ? t.moved_m / t.seconds : 0.0, (unsigned long long)t.starts,
				(unsigned long long)t.stops);
	};
	const AudioIngameLegTally &stand = legs_[size_t(AudioIngameLeg::Stand)];
	const AudioIngameLegTally &walk = legs_[size_t(AudioIngameLeg::Walk)];
	const AudioIngameLegTally &sprint = legs_[size_t(AudioIngameLeg::Sprint)];
	double walk_interval = 0.0, run_interval = 0.0, walk_clip = 0.0, run_clip = 0.0;
	size_t run_steps = 0, walk_known = 0, run_known = 0;
	for (size_t i = 0; i < walk.intervals.size(); ++i) {
		walk_interval += walk.intervals[i].second / double(walk.intervals.size());
		walk_clip += walk.clip_intervals[i] / double(walk.intervals.size());
		walk_known += walk.clip_intervals[i] > 0.0 ? 1u : 0u;
	}
	for (size_t i = 0; i < sprint.intervals.size(); ++i) {
		if (sprint.intervals[i].first > walk.speed_max) {
			run_interval += sprint.intervals[i].second;
			run_clip += sprint.clip_intervals[i];
			run_known += sprint.clip_intervals[i] > 0.0 ? 1u : 0u;
			++run_steps;
		}
	}
	run_interval = run_steps > 0 ? run_interval / double(run_steps) : 0.0;
	run_clip = run_steps > 0 ? run_clip / double(run_steps) : 0.0;
	const auto on_clip = [](double p_interval, double p_clip, size_t p_known, size_t p_intervals) {
		return p_intervals > 0 && p_known == p_intervals &&
				std::fabs(p_interval / p_clip - 1.0) <= kAudioIngameClipCadenceShare;
	};
	const bool walk_animated = walk.animated > 0 && walk.animated == walk.steps;
	const bool walk_cadence = walk.animated == 0 ||
			(walk_animated && on_clip(walk_interval, walk_clip, walk_known, walk.intervals.size()));
	row("own steps walking",
			walk.legs >= kAudioIngameMinLegs && walk.steps >= kAudioIngameMinLegSteps * walk.legs && walk_cadence,
			tally_text(AudioIngameLeg::Walk) +
					core::fmt(
							"; %u moving legs each along the clearest of %u yaws, the shortest clear %.1f of %.0f m; "
							"%u moves to the most open spawn point (%.1f m clear); steps every %.3f s, the run clip's "
							"step tags every %.3f s at the step speeds (%zu of %zu known; %s)",
							run_picks_, kAudioIngameRunYaws, double(run_clear_min_m_), double(kAudioIngameRunMeters),
							teleports_, double(spot_clear_m_), walk_interval, walk_clip, walk_known,
							walk.intervals.size(),
							walk.animated == 0		? "on the period, no clip cadence"
									: walk_animated ? "on the 1p animation, they must match within the share"
													: "period and animation steps mixed"));
	const bool on_period = walk.animated == 0 && sprint.animated == 0;
	const bool on_animation = walk_animated && sprint.animated == sprint.steps;
	const bool gait_cadence = on_period ? run_interval < walk_interval
										: on_animation && on_clip(run_interval, run_clip, run_known, run_steps);
	row("own steps sprinting",
			sprint.legs >= kAudioIngameMinLegs && sprint.steps >= kAudioIngameMinLegSteps * sprint.legs &&
					run_steps >= kAudioIngameMinLegSteps && sprint.speed_max > walk.speed_max && gait_cadence,
			tally_text(AudioIngameLeg::Sprint) +
					core::fmt("; %zu sprint steps faster than any walking step come every %.3f s, the sprint clip's "
							  "step tags every %.3f s (%zu of %zu known), walking steps every %.3f s; %s; against ",
							run_steps, run_interval, run_clip, run_known, run_steps, walk_interval,
							on_period ? "on the period, sprint steps must come sooner"
									: on_animation
									? "on the 1p animation, sprint steps must match the sprint clip within the share"
									: "period and animation steps mixed") +
					tally_text(AudioIngameLeg::Walk) + core::fmt("; the share %.3f", kAudioIngameClipCadenceShare));
	row("no own steps standing", stand.legs >= kAudioIngameMinLegs && stand.steps == 0,
			tally_text(AudioIngameLeg::Stand) +
					core::fmt(" (after the first %.1f s of each leg)", kAudioIngameLegSettleSeconds));
	row("own starts and stops",
			walk.starts + sprint.starts >= walk.legs + sprint.legs &&
					stand.stops + walk.stops + sprint.stops >= stands_after_moving_,
			core::fmt("%llu Start posts over %u moving legs, %llu Stop posts (%llu while standing) for %u standing "
					  "legs that follow a moving one",
					(unsigned long long)(walk.starts + sprint.starts), walk.legs + sprint.legs,
					(unsigned long long)(stand.stops + walk.stops + sprint.stops), (unsigned long long)stand.stops,
					stands_after_moving_));
	const GameAudioStats now = r_audio.stats();
	const ba::FootstepStats &a = stats_at_start_.footsteps;
	const ba::FootstepStats &b = now.footsteps;
	std::string ids;
	uint64_t materials = 0;
	for (size_t i = 0; i < b.by_material.size(); ++i) {
		const uint64_t n = b.by_material[i] - a.by_material[i];
		if (n != 0) {
			ids += core::fmt(" %zu:%llu", i, (unsigned long long)n);
			++materials;
		}
	}
	const std::string program = r_audio.footstep_program();
	uint64_t requested = 0, refused = 0, killed = 0;
	for (const auto &[h, f] : fates_) {
		if (!program.empty() && f.program == program) {
			++requested;
			refused += f.refused ? 1u : 0u;
			killed += f.killed ? 1u : 0u;
		}
	}
	row("footstep sounds",
			!program.empty() && requested > 0 && materials >= 1 && b.refused == a.refused && refused == 0 &&
					killed == 0,
			core::fmt("program %s: %llu instances requested in the recording, %llu refused, %llu killed; the driver "
					  "%llu refused, %llu without ground, by SoundMaterialId%s; %llu other soldiers' posts, farthest "
					  "%.1f m",
					program.empty() ? "none" : program.c_str(), (unsigned long long)requested,
					(unsigned long long)refused, (unsigned long long)killed,
					(unsigned long long)(b.refused - a.refused), (unsigned long long)(b.no_ground - a.no_ground),
					ids.empty() ? " none" : ids.c_str(), (unsigned long long)other_steps_, double(b.farthest_m)));
}

void AudioIngameCheck::analyse_flyby(Bf3GameAudio &r_audio) {
	const std::vector<ba::FlyBySound> sounds = r_audio.flyby_sounds();
	float radius = 0.0f;
	std::vector<std::string> programs;
	for (const ba::FlyBySound &f : sounds) {
		if (f.program) {
			radius = std::max(radius, f.radius_m);
			programs.push_back(f.program->name);
		}
	}
	const GameAudioStats now = r_audio.stats();
	const ba::FlyByAudioStats &a = stats_at_start_.flybys;
	const ba::FlyByAudioStats &b = now.flybys;
	const uint64_t bullets = b.bullets - a.bullets, starts = b.starts - a.starts;
	row("fly-bys start", starts > 0 && starts <= bullets,
			core::fmt("%llu presented bullets, %llu fly-by starts, %llu silent, %llu too slow, %llu receding, %llu "
					  "refused, %llu untracked in the recording; %u moves to %.0f m in front of the nearest enemy, the "
					  "latest %.1f s into a life, %u own deaths",
					(unsigned long long)bullets, (unsigned long long)starts, (unsigned long long)(b.silent - a.silent),
					(unsigned long long)(b.slow - a.slow), (unsigned long long)(b.receding - a.receding),
					(unsigned long long)(b.refused - a.refused), (unsigned long long)(b.untracked - a.untracked),
					fights_, double(kAudioIngameFightMeters), fight_after_s_, deaths_));
	uint64_t requested = 0, far = 0, refused = 0, started = 0;
	float farthest = 0.0f;
	for (const auto &[h, f] : fates_) {
		if (std::find(programs.begin(), programs.end(), f.program) == programs.end()) {
			continue;
		}
		++requested;
		started += f.started ? 1u : 0u;
		refused += f.refused ? 1u : 0u;
		far += f.distance_m > radius ? 1u : 0u;
		farthest = std::max(farthest, f.distance_m);
	}
	row("fly-by sounds within the data's radius",
			requested > 0 && far == 0 && refused == 0 && b.farthest_start_m <= radius,
			core::fmt("%zu fly-by programs, the largest FlyBySoundRadius %.1f m: %llu requested, %llu started, %llu "
					  "refused, %llu beyond the radius, farthest %.1f m (the driver's farthest start %.1f m)",
					programs.size(), double(radius), (unsigned long long)requested, (unsigned long long)started,
					(unsigned long long)refused, (unsigned long long)far, double(farthest),
					double(b.farthest_start_m)));
}

std::string AudioIngameCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += l + "\n";
	}
	out += core::fmt(
			"AUDIO INGAME: %s (%u rows, %u failed)", done_ && failures_ == 0 ? "PASS" : "FAIL", rows_, failures_);
	return out;
}

}
