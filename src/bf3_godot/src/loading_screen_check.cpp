#include "loading_screen_check.h"

#include "native_flow.h"
#include "native_loading.h"
#include "native_screens.h"

#include "bf3/core/util/format.h"
#include "bf3/host/ui_flow_session.h"

#include <cmath>

namespace bf3::godot_side {

using bf3::core::fmt;

void LoadingScreenCheck::fail(std::string p_why) {
	if (failure_.empty()) {
		failure_ = std::move(p_why);
	}
}

void LoadingScreenCheck::collect(Bf3UiScreens &r_screens, NativeFlow &r_flow) {
	const auto take = [this](std::string p_line) {
		if (lines_.size() < kLoadingCheckMaxLines) {
			lines_.push_back(std::move(p_line));
		}
	};
	if (bf3::host::UiFlowSession *s = r_flow.session()) {
		for (const std::string &l : s->log()) {
			if (!l.starts_with("logic ")) {
				take("flow " + l);
			}
		}
		s->clear_log();
	}
	for (const std::string &l : r_screens.log()) {
		take("screens " + l);
	}
	r_screens.clear_log();
}

bool LoadingScreenCheck::seen(std::string_view p_line) const {
	for (const std::string &l : lines_) {
		if (l.starts_with(p_line)) {
			return true;
		}
	}
	return false;
}

void LoadingScreenCheck::check_load(const NativeLoading &p_loading, const LoadingScreenCheckIo &p_io) {
	const NativeLoadingFacts &f = p_loading.facts();
	load_detail_ = "opened at update " + std::to_string(f.open_update) + ", " + fmt("%.3f", f.open_s - p_io.ready_s) +
			" s after _ready, during stage " + f.stage_at_open +
			(f.decode_done_at_open ? " (decode done)" : " (decode running)") + "; drawn on " +
			std::to_string(f.drawn_updates) + " updates; removed at update " + std::to_string(f.removed_update) + ", " +
			fmt("%.3f", f.removed_s - p_io.done_s) + " s from done, " + fmt("%.3f", p_io.done_s - p_io.ready_s) +
			" s after _ready; header '" + f.header + "', subheader '" + f.subheader + "', message '" + f.message +
			"', hint '" + f.hint + "'";
	if (!p_loading.error().empty()) {
		fail("the loading screen failed: " + p_loading.error());
	} else if (f.opens != 1) {
		fail("the loading screen opened " + std::to_string(f.opens) + " times during the load");
	} else if (f.decode_done_at_open) {
		fail("the loading screen opened only after the level decode finished");
	} else if (f.removed_update == 0 || p_loading.shown()) {
		fail("the loading screen is still up after the load completed");
	} else if (std::abs(f.removed_s - p_io.done_s) > 1.0e-3) {
		fail("the loading screen left " + fmt("%.3f", f.removed_s - p_io.done_s) + " s from the done frame");
	} else if (f.drawn_updates == 0 || f.drawn_updates < f.removed_update - f.open_update) {
		fail("the loading screen was drawn on " + std::to_string(f.drawn_updates) + " of " +
				std::to_string(f.removed_update - f.open_update + 1) + " updates");
	} else if (f.header != kLoadingCheckHeader || f.subheader != kLoadingCheckLevelName) {
		fail("header '" + f.header + "', subheader '" + f.subheader + "'");
	} else if (f.hint.empty()) {
		fail("no hint shown");
	} else if (f.message != kLoadingCheckWaiting && f.message != kLoadingCheckLevelName) {
		fail("LoadingMessage shows '" + f.message + "'");
	}
}

void LoadingScreenCheck::hints(const NativeLoading &p_loading, double p_now_s, const LoadingScreenCheckIo &p_io) {
	const NativeLoadingFacts &f = p_loading.facts();
	const double t = p_now_s - open_s_;
	if (messages_.empty() || messages_.back().text != f.message) {
		messages_.push_back({ f.message, t });
	}
	for (LoadingCheckShot &s : shots_) {
		if (!s.taken && t >= s.at_s) {
			s.taken = true;
			if (!shot_dir_.empty()) {
				const std::string path = shot_dir_ + "/loading_" + s.name + ".png";
				if (p_io.shot(path)) {
					shots_taken_.push_back(path + " at " + fmt("%.3f", t) + " s");
				}
			}
		}
	}
	if (f.hints_served == 2 && hint_changed_s_ < 0.0) {
		hint_changed_s_ = t;
		hint_next_ = f.hint;
	} else if (f.hints_served > 2) {
		fail("hint served " + std::to_string(f.hints_served) + " times by " + fmt("%.3f", t) + " s");
	}
	if (t < kLoadingCheckHintSeconds - kLoadingCheckHintSlackSeconds &&
			(f.hints_served != 1 || f.hint != hint_first_)) {
		fail("the hint changed at " + fmt("%.3f", t) + " s, before 13 s");
	}
	if (t < kLoadingCheckReopenSeconds) {
		return;
	}
	if (hint_changed_s_ < 0.0) {
		fail("the hint did not change in " + fmt("%.3f", t) + " s");
	} else if (std::abs(hint_changed_s_ - kLoadingCheckHintSeconds) > kLoadingCheckHintSlackSeconds) {
		fail("the hint changed at " + fmt("%.3f", hint_changed_s_) + " s, not 13 s");
	} else if (hint_next_.empty() || hint_next_ == hint_first_) {
		fail("the next hint '" + hint_next_ + "' is not a new hint");
	} else if (messages_.size() < 3 || messages_[0].text != kLoadingCheckWaiting ||
			messages_[1].text != kLoadingCheckLevelName) {
		fail("LoadingMessage did not show LOADING, then the level name, then alternate");
	} else if (messages_[1].at_s < kLoadingCheckLevelNameSeconds) {
		fail("LoadingMessage showed the level name at " + fmt("%.3f", messages_[1].at_s) + " s, before frame 68");
	} else {
		for (size_t i = 2; i < messages_.size(); ++i) {
			if (messages_[i].text != (i % 2 == 0 ? kLoadingCheckWaiting : kLoadingCheckLevelName)) {
				fail("LoadingMessage showed '" + messages_[i].text + "' at " + fmt("%.3f", messages_[i].at_s) + " s");
			}
		}
	}
	p_io.close();
	phase_ = Phase::Deploy;
	phase_s_ = p_now_s;
}

void LoadingScreenCheck::deploy(double p_now_s, const LoadingScreenCheckIo &p_io) {
	if (!seen("screens entered spawnscreenpc") || p_now_s - phase_s_ < kLoadingCheckHoldSeconds) {
		return;
	}
	if (!enter_sent_) {
		released_on_spawn_screen_ = !p_io.captured();
		if (!released_on_spawn_screen_) {
			fail("the mouse is captured on the spawn screen");
			return;
		}
		p_io.press(godot::KEY_ENTER, true);
		p_io.press(godot::KEY_ENTER, false);
		enter_sent_ = true;
		return;
	}
	if (seen("flow action Spawn(") && seen("screens entered hudmpscreen")) {
		phase_ = Phase::Spawned;
		spawned_s_ = p_now_s;
	}
}

bool LoadingScreenCheck::frame(const NativeLoading &p_loading, Bf3UiScreens &r_screens, NativeFlow &r_flow,
		double p_now_s, const LoadingScreenCheckIo &p_io) {
	if (phase_ == Phase::Done) {
		return true;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
	}
	collect(r_screens, r_flow);
	switch (phase_) {
		case Phase::Load:
			check_load(p_loading, p_io);
			if (failure_.empty()) {
				p_io.reopen();
				phase_ = Phase::Reopen;
				phase_s_ = p_now_s;
			}
			break;
		case Phase::Reopen:
			if (p_loading.open()) {
				open_s_ = p_loading.facts().open_s;
				hint_first_ = p_loading.facts().hint;
				phase_ = Phase::Hints;
				if (hint_first_.empty()) {
					fail("the reopened loading screen shows no hint");
				}
			} else if (!p_loading.error().empty() || p_now_s - phase_s_ > kLoadingCheckReopenSeconds) {
				fail("the loading screen did not reopen: " + p_loading.error());
			}
			break;
		case Phase::Hints:
			hints(p_loading, p_now_s, p_io);
			break;
		case Phase::Deploy:
			deploy(p_now_s, p_io);
			break;
		case Phase::Spawned:
			if (p_io.captured()) {
				passed_ = true;
				phase_ = Phase::Done;
			} else if (p_now_s - spawned_s_ > kLoadingCheckCaptureSeconds) {
				fail("the mouse was not captured " + fmt("%.3f", kLoadingCheckCaptureSeconds) + " s after the deploy");
			}
			break;
		case Phase::Done:
			break;
	}
	if (!r_flow.failed() && r_screens.errors().empty() && p_now_s - start_s_ > kLoadingCheckSeconds) {
		fail("timed out");
	}
	if (r_flow.failed()) {
		fail("the UI flow did not load: " + r_flow.error());
	} else if (!r_screens.errors().empty()) {
		fail("screen error: " + r_screens.errors().front());
	}
	if (!failure_.empty()) {
		passed_ = false;
		phase_ = Phase::Done;
	}
	return phase_ == Phase::Done;
}

std::string LoadingScreenCheck::report() const {
	std::string out = "[loading] load: " + load_detail_ + "\n";
	out += "[loading] reopen: hint '" + hint_first_ + "' then '" + hint_next_ + "' at " + fmt("%.3f", hint_changed_s_) +
			" s; LoadingMessage showed";
	for (const LoadingCheckMessage &m : messages_) {
		out += " '" + m.text + "' at " + fmt("%.3f", m.at_s) + " s";
	}
	out += "\n[loading] deploy: mouse released on the spawn screen " +
			std::string(released_on_spawn_screen_ ? "yes" : "no") + ", Enter sent " + (enter_sent_ ? "yes" : "no") +
			", spawned " + (spawned_s_ >= 0.0 ? "yes" : "no") + "\n";
	for (const std::string &s : shots_taken_) {
		out += "[loading] shot " + s + "\n";
	}
	if (!passed_) {
		for (const std::string &l : lines_) {
			out += "[loading]   trace " + l + "\n";
		}
	}
	out += passed_ ? "LOADING SCREEN: PASS" : "LOADING SCREEN: FAIL " + failure_;
	return out;
}

}
