#include "native_screens_check.h"

#include "native_flow.h"
#include "native_hud.h"
#include "native_screens.h"

#include <cmath>

namespace bf3::godot_side {

void NativeScreensCheck::take(std::string p_line, double p_now_s) {
	if (lines_.size() < kNativeScreensCheckMaxLines) {
		lines_.push_back({ p_now_s, std::move(p_line) });
	}
}

void NativeScreensCheck::collect(Bf3UiScreens &r_screens, NativeFlow &r_flow, double p_now_s) {
	if (bf3::host::UiFlowSession *s = r_flow.session()) {
		for (const std::string &l : s->log()) {
			if (!l.starts_with("logic ")) {
				take("flow " + l, p_now_s);
			}
		}
		s->clear_log();
	}
	for (const std::string &l : r_screens.log()) {
		take("screens " + l, p_now_s);
	}
	r_screens.clear_log();
}

double NativeScreensCheck::delay_seconds(Delay p_delay, const NativeScreensCheckIo &p_io) {
	switch (p_delay) {
		case Delay::KillCard:
			return double(p_io.death.kill_card_s);
		case Delay::SpawnScreens:
			return double(p_io.death.spawn_screens_s);
		case Delay::None:
			break;
	}
	return kNativeScreensCheckHoldSeconds;
}

void NativeScreensCheck::advance(const Bf3UiScreens &p_screens, double p_now_s, const NativeScreensCheckIo &p_io) {
	while (next_ < steps_.size()) {
		Step &step = steps_[next_];
		const std::string_view want = step.line;
		size_t at = cursor_;
		while (at < lines_.size() && !std::string_view(lines_[at].text).starts_with(want)) {
			++at;
		}
		if (step.absent) {
			const double wait = delay_seconds(step.delay, p_io);
			if (at != lines_.size()) {
				failure_ = "'" + std::string(want) + "' within " + std::to_string(wait) + " s of the step before";
				return;
			}
			if (p_now_s - steps_[next_ - 1].seen_s < wait) {
				return;
			}
			step.seen_s = p_now_s;
			if (step.widgets_back) {
				const SoldierWidgets w = soldier_widgets(p_screens);
				back_widgets_detail_ = w.detail;
				back_widgets_shown_ = w.shown == kNativeScreensCheckSoldierWidgets.size();
			}
			++next_;
			continue;
		}
		if (at == lines_.size()) {
			return;
		}
		step.seen_s = lines_[at].t;
		cursor_ = at + 1;
		if (step.timed_from != 0 && step.timed_from <= next_) {
			const double took = step.seen_s - steps_[next_ - step.timed_from].seen_s;
			const double want_s = delay_seconds(step.delay, p_io);
			if (std::fabs(took - want_s) > kNativeScreensCheckDelayToleranceSeconds) {
				failure_ = "'" + std::string(want) + "' " + std::to_string(took) + " s after '" +
						steps_[next_ - step.timed_from].line + "', the install's delay is " + std::to_string(want_s) +
						" s";
				return;
			}
		}
		++next_;
	}
}

void NativeScreensCheck::act(Step &r_step, const NativeScreensCheckIo &p_io, double p_now_s) {
	r_step.sent = true;
	take("check " + std::string(r_step.act_name), p_now_s);
	switch (r_step.act) {
		case Act::Press:
			p_io.press(r_step.press, true);
			p_io.press(r_step.press, false);
			break;
		case Act::Suicide:
			p_io.suicide();
			break;
		case Act::Revive:
			p_io.revive();
			break;
		case Act::None:
			break;
	}
}

NativeScreensCheck::SoldierWidgets NativeScreensCheck::soldier_widgets(const Bf3UiScreens &p_screens) {
	SoldierWidgets out;
	for (const std::string_view fact : kNativeScreensCheckSoldierWidgets) {
		std::string value = "never sent";
		for (const std::string_view partition : kNativeHudPartitions) {
			const std::optional<UiScreenRecord> r = p_screens.screen_record(partition);
			if (!r) {
				continue;
			}
			const auto it = r->numbers.find(fact);
			if (it != r->numbers.end()) {
				++out.sent;
				out.shown += it->second != 0.0 ? 1u : 0u;
				value = it->second != 0.0 ? "visible" : "hidden";
				break;
			}
		}
		out.detail += std::string(out.detail.empty() ? "" : ", ") + std::string(fact) + " " + value;
	}
	return out;
}

void NativeScreensCheck::watch_card(const Bf3UiScreens &p_screens) {
	const std::optional<UiScreenRecord> card = p_screens.screen_record(bf3::core::ui::kKillScreenPartition);
	const bool drawn_last_frame = card_shown_;
	card_shown_ = card && card->visible;
	if (!drawn_last_frame) {
		return;
	}
	++card_frames_;
	const SoldierWidgets w = soldier_widgets(p_screens);
	if (w.sent != kNativeScreensCheckSoldierWidgets.size() || w.shown != 0) {
		++card_frames_widgets_shown_;
		if (card_widgets_detail_.empty()) {
			card_widgets_detail_ = w.detail;
		}
	}
}

bool NativeScreensCheck::frame(
		Bf3UiScreens &r_screens, NativeFlow &r_flow, double p_now_s, const NativeScreensCheckIo &p_io) {
	if (done_) {
		return true;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
	}
	load_seconds_ = r_flow.load_seconds();
	actions_ = r_flow.actions();
	collect(r_screens, r_flow, p_now_s);
	death_ = p_io.death;
	advance(r_screens, p_now_s, p_io);
	watch_card(r_screens);
	if (failure_.empty() && next_ < steps_.size() && next_ > 0 && steps_[next_].act != Act::None &&
			!steps_[next_].sent && p_now_s - steps_[next_ - 1].seen_s >= kNativeScreensCheckHoldSeconds) {
		act(steps_[next_], p_io, p_now_s);
		collect(r_screens, r_flow, p_now_s);
		advance(r_screens, p_now_s, p_io);
	}
	if (!failure_.empty()) {
		done_ = true;
	} else if (r_flow.failed()) {
		failure_ = "the UI flow did not load: " + r_flow.error();
	} else if (!r_screens.errors().empty()) {
		failure_ = "screen error: " + r_screens.errors().front();
	} else if (r_flow.session() != nullptr && !r_flow.session()->errors().empty()) {
		failure_ = "flow session error: " + r_flow.session()->errors().front();
	} else if (r_flow.session() != nullptr && !r_flow.session()->flow().errors().empty()) {
		failure_ = "flow error: " + r_flow.session()->flow().errors().front();
	} else if (next_ < steps_.size() && p_now_s - start_s_ > kNativeScreensCheckSeconds) {
		failure_ = std::string("timed out waiting for '") + steps_[next_].line + "'";
	} else if (next_ == steps_.size()) {
		if (card_frames_ == 0) {
			failure_ = "the killscreen was never visible";
		} else if (card_frames_widgets_shown_ != 0) {
			failure_ = std::to_string(card_frames_widgets_shown_) + " of " + std::to_string(card_frames_) +
					" killscreen frames did not send every soldier widget hidden, first: " + card_widgets_detail_;
		} else if (!back_widgets_shown_) {
			failure_ = "after the revive the soldier widgets are not all visible: " + back_widgets_detail_;
		}
	}
	if (!failure_.empty() || next_ == steps_.size()) {
		done_ = true;
		passed_ = failure_.empty();
	}
	return done_;
}

std::string NativeScreensCheck::report() const {
	std::string out = "[ui] native screens check: flow loaded in " + std::to_string(load_seconds_) + " s, " +
			std::to_string(actions_) + " flow actions, install death timeline: kill card " +
			std::to_string(death_.kill_card_s) + " s, spawn screens " + std::to_string(death_.spawn_screens_s) + " s\n";
	for (size_t i = 0; i < steps_.size(); ++i) {
		const Step &s = steps_[i];
		out += "[ui]   ";
		if (s.act_name != nullptr) {
			out += std::string(s.act == Act::Press ? "press " : "") + s.act_name + ", ";
		}
		out += std::string(s.absent ? "no " : "") + s.line + ": ";
		out += s.seen_s >= 0.0 ? "at " + std::to_string(s.seen_s - start_s_) + " s" : std::string("not seen");
		if (s.timed_from != 0 && s.timed_from <= i && s.seen_s >= 0.0 && steps_[i - s.timed_from].seen_s >= 0.0) {
			const Step &from = steps_[i - s.timed_from];
			out += ", " + std::to_string(s.seen_s - from.seen_s) + " s after '" + from.line + "'";
		}
		out += "\n";
	}
	out += "[ui]   killscreen visible on " + std::to_string(card_frames_) +
			" frames, soldier widgets not all hidden on " + std::to_string(card_frames_widgets_shown_) +
			(card_widgets_detail_.empty() ? std::string() : " (" + card_widgets_detail_ + ")") +
			"; after the revive: " + back_widgets_detail_ + "\n";
	if (!passed_) {
		for (const Line &l : lines_) {
			out += "[ui]   trace " + std::to_string(l.t - start_s_) + " " + l.text + "\n";
		}
	}
	out += passed_ ? "NATIVE SCREENS: PASS" : "NATIVE SCREENS: FAIL " + failure_;
	return out;
}

}
