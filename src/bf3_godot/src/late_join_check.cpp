#include "late_join_check.h"

#include "bf3/core/util/format.h"
#include "bf3/sim/weapons/sim_time.h"

#include <algorithm>

namespace bf3::godot_side {

namespace {

const char *phase_text(net::MatchPhase p_phase) {
	switch (p_phase) {
		case net::MatchPhase::Loading:
			return "Loading";
		case net::MatchPhase::PreRound:
			return "PreRound";
		case net::MatchPhase::InRound:
			return "InRound";
		case net::MatchPhase::RoundOver:
			return "RoundOver";
		case net::MatchPhase::Restarting:
			return "Restarting";
		case net::MatchPhase::LevelChange:
			return "LevelChange";
	}
	return "?";
}

}

std::optional<LateJoinMode> LateJoinCheck::parse_mode(const std::string &p_text) {
	if (p_text == "in-round") {
		return LateJoinMode::InRound;
	}
	if (p_text == "round-over") {
		return LateJoinMode::RoundOver;
	}
	return std::nullopt;
}

bool LateJoinCheck::new_effect_frame(uint64_t p_frames) {
	const bool fresh = p_frames != effect_frames_;
	effect_frames_ = p_frames;
	return fresh;
}

void LateJoinCheck::note_starts(const LateJoinFrame &p_frame) {
	for (const fx::TriggerStart &s : p_frame.effect_starts) {
		++effect_starts_;
		if (stale_events_.count({ uint8_t(s.kind), s.slot, s.id }) > 0) {
			++stale_effect_starts_;
		}
	}
	for (const audio::WeaponSoundStart &s : p_frame.sound_starts) {
		++sound_starts_;
		if (present_s_ >= 0.0 &&
				(s.kind == audio::WeaponSoundStartKind::RemoteOneShot ||
						s.kind == audio::WeaponSoundStartKind::RemoteBurst ||
						s.kind == audio::WeaponSoundStartKind::StateBurst)) {
			fire_sound_s_.emplace(s.slot, p_frame.now_s);
		}
		switch (s.kind) {
			case audio::WeaponSoundStartKind::RemoteOneShot:
			case audio::WeaponSoundStartKind::Detonation:
				if (stale_sounds_.count({ s.slot, s.key }) > 0) {
					++stale_sound_starts_;
				}
				break;
			case audio::WeaponSoundStartKind::RemoteBurst:
				++remote_bursts_;
				if (previous_stale_bursts_.count({ s.slot, s.def }) > 0) {
					++stale_burst_starts_;
				}
				break;
			case audio::WeaponSoundStartKind::StateBurst:
				++state_bursts_;
				break;
			default:
				break;
		}
	}
}

void LateJoinCheck::note_events(const LateJoinFrame &p_frame) {
	const bool waiting = p_frame.first_snapshot_tick == 0;
	const sim::weapons::SimTime first = waiting ? 0 : sim::weapons::tick_start(p_frame.first_snapshot_tick);
	stale_bursts_.clear();
	std::set<BurstKey> fresh_bursts;
	for (const present::PresentEvent &e : p_frame.events) {
		if (e.timeline != present::PresentTimeline::Render) {
			continue;
		}
		++render_events_;
		const bool stale = waiting || e.time < first;
		if (!stale) {
			if (e.kind == present::PresentKind::RemoteShot) {
				fresh_bursts.insert({ e.slot, e.def });
				if (in_loop_window(p_frame.now_s) && e.slot != p_frame.own_slot) {
					firing_at_join_.emplace(e.slot, p_frame.now_s);
				}
			}
			continue;
		}
		++stale_count_;
		stale_events_.insert({ uint8_t(e.kind), e.slot, e.id });
		if (e.kind == present::PresentKind::RemoteShot) {
			stale_sounds_.insert({ e.slot, e.id });
			stale_bursts_.insert({ e.slot, e.def });
		} else if (e.kind == present::PresentKind::Detonation) {
			stale_sounds_.insert({ e.slot, kLateJoinDetonationKey | (e.id & 0x3FFFFFFFu) });
		}
	}
	for (const BurstKey &k : fresh_bursts) {
		stale_bursts_.erase(k);
	}
	previous_stale_bursts_ = stale_bursts_;
}

void LateJoinCheck::note_logs(const LateJoinFrame &p_frame) {
	const auto keep = [this](const char *p_from, const std::string &p_line) {
		if (notes_.size() < kLateJoinMaxNotes &&
				(p_line.find("RoundOver") != std::string::npos || p_line.find("ndOfRound") != std::string::npos ||
						p_line.find("eor") != std::string::npos || p_line.find("scoreboard") != std::string::npos)) {
			notes_.push_back(std::string(p_from) + " " + p_line);
		}
	};
	for (const std::string &l : p_frame.flow_log) {
		keep("flow", l);
	}
	for (const std::string &l : p_frame.screens_log) {
		keep("screens", l);
	}
}

void LateJoinCheck::note_join(const LateJoinFrame &p_frame) {
	join_s_ = p_frame.now_s;
	join_tick_ = p_frame.first_snapshot_tick;
	join_phase_valid_ = p_frame.match_valid;
	join_phase_ = p_frame.phase;
	frame_valid_at_join_ = p_frame.frame_valid;
	others_at_join_ = p_frame.others.size();
}

void LateJoinCheck::fail(std::string p_why) {
	if (failure_.empty()) {
		failure_ = std::move(p_why);
	}
}

bool LateJoinCheck::verdict() {
	done_ = true;
	const net::MatchPhase wanted =
			mode_ == LateJoinMode::InRound ? net::MatchPhase::InRound : net::MatchPhase::RoundOver;
	if (!join_phase_valid_ || join_phase_ != wanted) {
		fail(std::string("joined in ") + (join_phase_valid_ ? phase_text(join_phase_) : "no match state") + ", not " +
				phase_text(wanted));
	}
	if (stale_effect_starts_ > 0) {
		fail(core::fmt("%llu effect starts for events timed before the first snapshot",
				(unsigned long long)stale_effect_starts_));
	}
	if (stale_sound_starts_ > 0 || stale_burst_starts_ > 0) {
		fail(core::fmt("%llu one-shot and %llu burst sound starts for events timed before the first snapshot",
				(unsigned long long)stale_sound_starts_, (unsigned long long)stale_burst_starts_));
	}
	if (mode_ == LateJoinMode::InRound) {
		if (effect_starts_ == 0) {
			fail(core::fmt("no effect started in the %.0f s after the join", kLateJoinPlaySeconds));
		}
		if (firing_at_join_.empty()) {
			fail(core::fmt("no remote soldier fired within %.1f s of the presentation start", kLateJoinLoopWindowS));
		} else if (!silent_.empty()) {
			fail(core::fmt("%zu of the %zu soldiers firing within %.1f s of the presentation start had no fire sound "
						   "%.1f s after",
					silent_.size(), firing_at_join_.size(), kLateJoinLoopWindowS, kLateJoinLoopWindowS));
		}
	} else if (eor_after_s_ < 0.0) {
		fail(core::fmt("no end-of-round screen within %.0f s of the join", kLateJoinEorSeconds));
	}
	return true;
}

bool LateJoinCheck::frame(const LateJoinFrame &p_frame) {
	if (done_) {
		return true;
	}
	if (first_frame_s_ < 0.0) {
		first_frame_s_ = p_frame.now_s;
	}
	if (join_s_ < 0.0 && p_frame.first_snapshot_tick != 0) {
		note_join(p_frame);
	}
	if (join_s_ >= 0.0 && present_s_ < 0.0 && p_frame.frame_valid) {
		present_s_ = p_frame.now_s;
	}
	if (in_loop_window(p_frame.now_s)) {
		for (const present::PresentedOtherWeapon &o : p_frame.others) {
			if (o.valid && o.firing && o.slot != p_frame.own_slot) {
				firing_at_join_.emplace(o.slot, p_frame.now_s);
			}
		}
	}
	note_starts(p_frame);
	note_events(p_frame);
	note_logs(p_frame);
	effects_ = p_frame.effects;
	router_stale_dropped_ = p_frame.router_stale_dropped;
	impact_stale_dropped_ = p_frame.impact_stale_dropped;
	if (join_s_ < 0.0) {
		if (p_frame.now_s - first_frame_s_ > kLateJoinFirstSnapshotS) {
			fail(core::fmt("no first snapshot within %.0f s", kLateJoinFirstSnapshotS));
			done_ = true;
			return true;
		}
		return false;
	}
	const double since = p_frame.now_s - join_s_;
	for (const present::PresentedOtherWeapon &o : p_frame.others) {
		if (o.valid && o.firing && o.slot != p_frame.own_slot) {
			firing_seen_.insert(o.slot);
		}
	}
	if (in_loop_window(p_frame.now_s)) {
		window_voices_ = std::max(window_voices_, p_frame.weapon_voices);
	}
	if (eor_after_s_ < 0.0 && p_frame.eor_visible) {
		eor_after_s_ = since;
	}
	if (mode_ == LateJoinMode::InRound) {
		if (present_s_ < 0.0) {
			if (since >= kLateJoinPresentS) {
				fail(core::fmt(
						"the presentation did not start within %.0f s of the first snapshot", kLateJoinPresentS));
				return verdict();
			}
			return false;
		}
		if (p_frame.now_s - present_s_ < kLateJoinPlaySeconds) {
			return false;
		}
		for (const auto &[slot, fired_s] : firing_at_join_) {
			const auto sound = fire_sound_s_.find(slot);
			if (sound == fire_sound_s_.end() || sound->second - fired_s > kLateJoinLoopWindowS) {
				silent_.push_back(slot);
			}
		}
		return verdict();
	}
	if ((eor_after_s_ >= 0.0 && since >= kLateJoinLoopWindowS) || since >= kLateJoinEorSeconds) {
		return verdict();
	}
	return false;
}

bool LateJoinCheck::in_loop_window(double p_now_s) const {
	return present_s_ >= 0.0 && p_now_s - present_s_ <= kLateJoinLoopWindowS;
}

std::string LateJoinCheck::report() const {
	std::string out =
			core::fmt("[late_join] LATE JOIN %s: %s", mode_ == LateJoinMode::InRound ? "in-round" : "round-over",
					failure_.empty() ? "PASS" : ("FAIL " + failure_).c_str());
	out += core::fmt(
			" | joined at tick %u in %s; %llu render events, %llu timed before the first snapshot (dropped: fx "
			"%llu, weapon audio %llu, impact audio %llu)",
			unsigned(join_tick_), join_phase_valid_ ? phase_text(join_phase_) : "no match state",
			(unsigned long long)render_events_, (unsigned long long)stale_count_,
			(unsigned long long)effects_.stale_dropped, (unsigned long long)router_stale_dropped_,
			(unsigned long long)impact_stale_dropped_);
	out += core::fmt(" | effect starts %llu (stale %llu), weapon sound starts %llu (stale one-shots %llu, stale bursts "
					 "%llu)",
			(unsigned long long)effect_starts_, (unsigned long long)stale_effect_starts_,
			(unsigned long long)sound_starts_, (unsigned long long)stale_sound_starts_,
			(unsigned long long)stale_burst_starts_);
	out += core::fmt(" | presentation started %.2f s after the first snapshot; within %.1f s of it %zu soldiers "
					 "fired, %zu of them with no fire sound %.1f s after, at most %u weapon voices",
			present_s_ >= 0.0 ? present_s_ - join_s_ : -1.0, kLateJoinLoopWindowS, firing_at_join_.size(),
			silent_.size(), kLateJoinLoopWindowS, unsigned(window_voices_));
	out += core::fmt(" | frame valid at the join %s, %zu others; soldiers seen firing %zu; weapon sound starts: "
					 "state bursts %llu, event bursts %llu",
			frame_valid_at_join_ ? "yes" : "no", others_at_join_, firing_seen_.size(),
			(unsigned long long)state_bursts_, (unsigned long long)remote_bursts_);
	out += core::fmt(
			" | fx: %llu events, %llu remote shots, %llu impacts, %llu culled, %llu rejected, %llu pending (%llu "
			"dropped)",
			(unsigned long long)effects_.events, (unsigned long long)effects_.remote_shots,
			(unsigned long long)effects_.impacts, (unsigned long long)effects_.culled,
			(unsigned long long)effects_.rejected, (unsigned long long)effects_.pending_now,
			(unsigned long long)effects_.pending_dropped);
	if (mode_ == LateJoinMode::RoundOver) {
		out += eor_after_s_ >= 0.0 ? core::fmt(" | end-of-round screen %.2f s after the join", eor_after_s_)
								   : std::string(" | no end-of-round screen");
		for (const std::string &n : notes_) {
			out += "\n[late_join]   " + n;
		}
	}
	return out;
}

}
