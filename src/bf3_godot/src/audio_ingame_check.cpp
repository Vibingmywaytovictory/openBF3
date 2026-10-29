#include "audio_ingame_check.h"

#include "game_audio.h"
#include "level_session_game.h"

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

namespace bf3::godot_side {

namespace ba = bf3::audio;

namespace {

constexpr size_t kCutBatch = 256;

bool silent_kind(ba::MixerCutKind p_kind) {
	switch (p_kind) {
		case ba::MixerCutKind::Start:
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

AudioSoundCategory audio_sound_category(std::string_view p_program, std::string_view p_scope, float p_distance_m) {
	const std::string program = core::to_lower_ascii(p_program);
	const std::string scope = core::to_lower_ascii(p_scope);
	if (program.find("bullet_impacts") != std::string::npos) {
		return program.find("soldier") != std::string::npos || scope.find("soldier") != std::string::npos
				? AudioSoundCategory::ImpactSoldier
				: AudioSoundCategory::ImpactSurface;
	}
	if (program.find("explosion") != std::string::npos || program.find("grenade") != std::string::npos) {
		return AudioSoundCategory::Explosion;
	}
	if (program.find("weapons/") != std::string::npos && program.find("foley") == std::string::npos) {
		return p_distance_m < kAudioIngameOwnMetres ? AudioSoundCategory::OwnGun : AudioSoundCategory::BotGun;
	}
	return AudioSoundCategory::Other;
}

AudioIngameCheck::AudioIngameCheck(std::string p_out_dir, double p_seconds) :
		out_dir_(std::move(p_out_dir)),
		seconds_(std::isfinite(p_seconds) && p_seconds > 0.0 ? std::clamp(p_seconds, 5.0, 120.0)
															 : kAudioIngameSeconds) {}

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
	heard_.reserve(size_t((seconds_ + 5.0) * double(ba::kMixRate)) * 2);
	events_.reserve(size_t(1) << 16);
	r_audio.mixer()->record_cuts(true);
	std::array<ba::MixerCutEvent, kCutBatch> batch;
	while (r_audio.mixer()->poll_cuts(batch) == batch.size()) {
	}
	at_start_ = snapshot(r_audio);
	as->add_bus_effect(0, capture_, -1);
	r_audio.set_mix_tap(mix_tap_.get());
	record_s_ = p_now_s;
}

void AudioIngameCheck::end(Bf3GameAudio &r_audio) {
	r_audio.set_mix_tap(nullptr);
	pull();
	godot::AudioServer *as = godot::AudioServer::get_singleton();
	for (int i = as->get_bus_effect_count(0) - 1; i >= 0; --i) {
		if (as->get_bus_effect(0, i) == capture_) {
			as->remove_bus_effect(0, i);
		}
	}
	r_audio.mixer()->record_cuts(false);
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

void AudioIngameCheck::note(const ba::MixerCutEvent &p_event, double p_now_s, Bf3GameAudio &r_audio) {
	events_.push_back(p_event);
	if (p_event.kind == ba::MixerCutKind::Request) {
		AudioSoundFate f;
		f.program = p_event.program;
		f.request_s = p_now_s - record_s_;
		f.distance_m = p_event.distance_m;
		f.scope = p_event.scope;
		f.category = audio_sound_category(f.program, r_audio.scope_name(p_event.scope), p_event.distance_m);
		fates_[p_event.handle] = std::move(f);
		return;
	}
	if (p_event.handle == ba::kNoInstance) {
		return;
	}
	if (p_event.kind == ba::MixerCutKind::Join) {
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

void AudioIngameCheck::drain(Bf3GameAudio &r_audio, double p_now_s) {
	std::array<ba::MixerCutEvent, kCutBatch> batch;
	for (;;) {
		const size_t n = r_audio.mixer()->poll_cuts(batch);
		for (size_t i = 0; i < n; ++i) {
			note(batch[i], p_now_s, r_audio);
		}
		if (n < batch.size()) {
			return;
		}
	}
}

void AudioIngameCheck::script(double p_now_s, const AudioIngameIo &p_io) {
	if (!p_io.alive) {
		if (firing_) {
			p_io.hold_fire(false);
			firing_ = false;
		}
		deaths_ += was_alive_ ? 1u : 0u;
		was_alive_ = false;
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
	}
	const double in = p_now_s - alive_s_;
	if (in < kAudioIngameSettleSeconds) {
		base_yaw_ = p_io.yaw();
		last_s_ = p_now_s;
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
	const bool want = t < kAudioIngameHoldSeconds;
	if (!want && firing_ && step % 2 == 1) {
		p_io.hold_fire(false);
		firing_ = false;
		p_io.reload();
	}
	if (want != firing_) {
		p_io.hold_fire(want);
		firing_ = want;
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
			pull();
			drain(r_audio, p_now_s);
			if (p_now_s - record_s_ < seconds_) {
				return false;
			}
			if (firing_) {
				p_io.hold_fire(false);
				firing_ = false;
			}
			end(r_audio);
			drain(r_audio, p_now_s);
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
	for (const ba::MixerCutEvent &e : events_) {
		if (e.kind == ba::MixerCutKind::CallGap) {
			++call_gaps;
			call_gap_max = std::max(call_gap_max, e.step);
		}
		if (!silent_kind(e.kind) && e.step > ba::kAudibleStep) {
			++audible[std::string(ba::mixer_cut_kind_name(e.kind)) + " " + e.program];
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
	row("no sound outlives its data", delta(&Snapshot::pool_forgotten) == 0 && delta(&Snapshot::router_forgotten) == 0,
			core::fmt("%llu one-shots and %llu weapon voices killed for never finishing",
					(unsigned long long)delta(&Snapshot::pool_forgotten),
					(unsigned long long)delta(&Snapshot::router_forgotten)));

	std::array<AudioCategoryCounts, size_t(AudioSoundCategory::Count)> cats;
	std::string fates_csv = "handle,category,request_s,distance_m,scope,started,finished,cull,park,parks,refused,"
							"killed,joined,coalesced,program\n";
	for (const auto &[h, f] : fates_) {
		AudioCategoryCounts &c = cats[size_t(f.category)];
		++c.requested;
		c.started += f.started ? 1u : 0u;
		c.finished += f.finished ? 1u : 0u;
		c.joined += f.joined ? 1u : 0u;
		if (!f.started && f.cull == 0 && f.parks == 0 && !f.refused && !f.killed) {
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
				std::string(kAudioSoundCategoryNames[size_t(f.category)]).c_str(), f.request_s, double(f.distance_m),
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
						std::string(kAudioSoundCategoryNames[k]).c_str(), (unsigned long long)c.requested,
						(unsigned long long)c.started, (unsigned long long)c.finished, (unsigned long long)c.joined,
						(unsigned long long)c.data_drops, (unsigned long long)c.other_drops) +
				reasons);
	}
	const uint64_t lost_counters = delta(&Snapshot::pool_stolen) + delta(&Snapshot::router_stolen) +
			delta(&Snapshot::event_drops) + delta(&Snapshot::ring_overflows) + delta(&Snapshot::feedback_drops) +
			delta(&Snapshot::router_refusals) + delta(&Snapshot::pool_failed);
	row("drops only by the data's rules", other_drops == 0 && lost_counters == 0,
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
	const AudioCategoryCounts &own = cats[size_t(AudioSoundCategory::OwnGun)];
	const AudioCategoryCounts &bots = cats[size_t(AudioSoundCategory::BotGun)];
	const AudioCategoryCounts &surface = cats[size_t(AudioSoundCategory::ImpactSurface)];
	row("the scenario ran", bursts_ >= 6 && own.requested >= 1 && bots.requested >= 10 && surface.requested >= 20,
			core::fmt("%.1f s recorded, %u own bursts, %u own deaths, %llu own gun, %llu bot gun and %llu surface "
					  "impact sounds requested",
					seconds_, bursts_, deaths_, (unsigned long long)own.requested, (unsigned long long)bots.requested,
					(unsigned long long)surface.requested));

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
