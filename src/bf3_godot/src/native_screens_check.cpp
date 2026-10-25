#include "native_screens_check.h"

#include "native_flow.h"
#include "native_screens.h"

namespace bf3::godot_side {

void NativeScreensCheck::take(std::string p_line, double p_now_s) {
	if (lines_.size() < kNativeScreensCheckMaxLines) {
		lines_.push_back(std::to_string(p_now_s - start_s_) + " " + std::move(p_line));
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

void NativeScreensCheck::advance(double p_now_s) {
	while (next_ < steps_.size()) {
		const std::string_view want = steps_[next_].line;
		size_t at = cursor_;
		while (at < lines_.size()) {
			const std::string_view l = lines_[at];
			const size_t space = l.find(' ');
			if (space != std::string_view::npos && l.substr(space + 1).starts_with(want)) {
				break;
			}
			++at;
		}
		if (at == lines_.size()) {
			return;
		}
		steps_[next_].seen_s = p_now_s;
		cursor_ = at + 1;
		++next_;
	}
}

bool NativeScreensCheck::frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, double p_now_s) {
	if (done_) {
		return true;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
	}
	load_seconds_ = r_flow.load_seconds();
	actions_ = r_flow.actions();
	collect(r_screens, r_flow, p_now_s);
	advance(p_now_s);
	if (next_ < steps_.size() && next_ > 0 && steps_[next_].press && !steps_[next_].sent &&
			p_now_s - steps_[next_ - 1].seen_s >= kNativeScreensCheckHoldSeconds) {
		Step &s = steps_[next_];
		s.sent = true;
		take("check presses " + std::string(bf3::core::ui::ui_input_concept_name(*s.press)), p_now_s);
		r_screens.input_concept(int32_t(*s.press), true);
		r_screens.input_concept(int32_t(*s.press), false);
		collect(r_screens, r_flow, p_now_s);
		advance(p_now_s);
	}
	if (r_flow.failed()) {
		failure_ = "the UI flow did not load: " + r_flow.error();
	} else if (!r_screens.errors().empty()) {
		failure_ = "screen error: " + r_screens.errors().front();
	} else if (r_flow.session() != nullptr && !r_flow.session()->errors().empty()) {
		failure_ = "flow session error: " + r_flow.session()->errors().front();
	} else if (r_flow.session() != nullptr && !r_flow.session()->flow().errors().empty()) {
		failure_ = "flow error: " + r_flow.session()->flow().errors().front();
	} else if (next_ < steps_.size() && p_now_s - start_s_ > kNativeScreensCheckSeconds) {
		failure_ = std::string("timed out waiting for '") + steps_[next_].line + "'";
	}
	if (!failure_.empty() || next_ == steps_.size()) {
		done_ = true;
		passed_ = failure_.empty();
	}
	return done_;
}

std::string NativeScreensCheck::report() const {
	std::string out = "[ui] native screens check: flow loaded in " + std::to_string(load_seconds_) + " s, " +
			std::to_string(actions_) + " flow actions\n";
	for (const Step &s : steps_) {
		out += "[ui]   " +
				std::string(
						s.press ? "press " + std::string(bf3::core::ui::ui_input_concept_name(*s.press)) + ", " : "") +
				s.line + ": " +
				(s.seen_s >= 0.0 ? "at " + std::to_string(s.seen_s - start_s_) + " s" : std::string("not seen")) + "\n";
	}
	if (!passed_) {
		for (const std::string &l : lines_) {
			out += "[ui]   trace " + l + "\n";
		}
	}
	out += passed_ ? "NATIVE SCREENS: PASS" : "NATIVE SCREENS: FAIL " + failure_;
	return out;
}

}
