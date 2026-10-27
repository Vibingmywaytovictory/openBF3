#include "overlay_check.h"

#include "native_screens.h"

#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/util/format.h"
#include "bf3/host/client_session.h"

#include <algorithm>

namespace bf3::godot_side {

namespace {

constexpr const char *kAlert = "instance1.HudAlertMessage_03";
constexpr const char *kAlertContent = "instance1.HudAlertMessage_03.alert.content.";
constexpr const char *kWaitingTitle = "WAITING FOR PLAYERS";
constexpr const char *kWaitingDescription = "PLAYERS NEEDED";
constexpr const char *kCountdownTitle = "GET READY";
constexpr const char *kCountdownDescription = "ROUND WILL START IN";

}

OverlayCheck::Box OverlayCheck::box(Bf3UiScreens &r_screens) {
	Box b;
	const std::optional<UiScreenRecord> record =
			r_screens.screen_record(bf3::core::ui::kPreRoundWaitingScreenPartition);
	bf3::core::ui::HudScreen *s = r_screens.hud_screen(bf3::core::ui::kPreRoundWaitingScreenPartition);
	if (!record || !record->visible || s == nullptr) {
		return b;
	}
	const std::string content = kAlertContent;
	b.shown = s->clip(kAlert).visible() && s->clip(std::string(kAlert) + ".alert").visible();
	b.title = s->clip(content + "title").text();
	b.description = s->clip(content + "description").text();
	b.timer = s->clip(content + "timer").text();
	return b;
}

bool OverlayCheck::scoreboard(const Bf3UiScreens &p_screens) {
	const std::optional<UiScreenRecord> r = p_screens.screen_record(bf3::core::ui::kScoreboardHud32ScreenPartition);
	return r && r->visible;
}

void OverlayCheck::pass(std::string p_line, double p_t) {
	lines_.push_back(bf3::core::fmt("%.2f s ", p_t) + p_line);
}

void OverlayCheck::fail(std::string p_why) {
	if (!failed_) {
		failed_ = true;
		failure_ = std::move(p_why);
	}
}

void OverlayCheck::shot(const char *p_name, double p_t, const OverlayCheckIo &p_io) {
	if (shot_dir_.empty() || !p_io.shot) {
		return;
	}
	const std::string path = shot_dir_ + "/" + p_name + ".png";
	if (p_io.shot(path)) {
		shots_.push_back(path + bf3::core::fmt(" at %.2f s", p_t));
	}
}

bool OverlayCheck::frame(
		Bf3UiScreens &r_screens, const bf3::host::PresentationFrame &p_f, double p_now_s, const OverlayCheckIo &p_io) {
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
	}
	const double t = p_now_s - start_s_;
	if (t > kOverlayCheckSeconds) {
		fail(bf3::core::fmt("timed out after %.0f s in stage %u", kOverlayCheckSeconds, unsigned(stage_)));
	}
	if (!failed_) {
		step(r_screens, p_f, t, p_io);
	}
	if (failed_) {
		p_io.press(godot::KEY_TAB, false);
		return true;
	}
	passed_ = stage_ == Stage::Done;
	return passed_;
}

void OverlayCheck::step(
		Bf3UiScreens &r_screens, const bf3::host::PresentationFrame &p_f, double p_t, const OverlayCheckIo &p_io) {
	const Box b = box(r_screens);
	const bool in_round = p_f.match_valid && p_f.match.phase == bf3::net::MatchPhase::InRound;
	switch (stage_) {
		case Stage::Waiting: {
			if (in_round) {
				fail("the round started before a second player joined");
				return;
			}
			const uint16_t needed = p_f.match_valid ? p_f.match.players_needed : 0;
			const bool waiting = b.shown && b.title == kWaitingTitle && b.description == kWaitingDescription &&
					needed > 0 && b.timer == std::to_string(needed);
			if (!waiting) {
				mark_s_ = -1.0;
				return;
			}
			if (mark_s_ < 0.0) {
				mark_s_ = p_t;
				pass(b.title + " / " + b.description + " " + b.timer, p_t);
				return;
			}
			if (p_t - mark_s_ < kOverlayCheckSettleSeconds) {
				return;
			}
			shot("overlay_waiting", p_t, p_io);
			std::string error;
			if (!p_io.join(&error)) {
				fail("second client: " + error);
				return;
			}
			pass("a second client joins", p_t);
			stage_ = Stage::Countdown;
			mark_s_ = -1.0;
			return;
		}
		case Stage::Countdown: {
			if (b.shown && b.title == kCountdownTitle && b.description == kCountdownDescription &&
					(counts_.empty() || counts_.back() != b.timer)) {
				counts_.push_back(b.timer);
				pass(b.title + " / " + b.description + " " + b.timer, p_t);
				if (b.timer == "2" && !shot_taken_) {
					shot_taken_ = true;
					shot("overlay_countdown", p_t, p_io);
				}
			}
			if (!in_round) {
				return;
			}
			const bool counted = std::find(counts_.begin(), counts_.end(), "2") != counts_.end() &&
					std::find(counts_.begin(), counts_.end(), "1") != counts_.end();
			if (!counted) {
				fail("the countdown did not show 2 and 1 before the round started");
				return;
			}
			pass("the round starts", p_t);
			stage_ = Stage::RoundStart;
			mark_s_ = p_t;
			return;
		}
		case Stage::RoundStart:
			if (b.shown) {
				if (p_t - mark_s_ > kOverlayCheckReactSeconds) {
					fail("the pre-round box still shows " + b.title + " " + b.timer + " after the round started");
				}
				return;
			}
			if (p_t - mark_s_ < kOverlayCheckSettleSeconds) {
				return;
			}
			pass("the pre-round box is gone", p_t);
			p_io.press(godot::KEY_TAB, true);
			stage_ = Stage::TabDown;
			mark_s_ = p_t;
			shot_taken_ = false;
			return;
		case Stage::TabDown:
			if (!scoreboard(r_screens)) {
				if (p_t - mark_s_ > kOverlayCheckReactSeconds) {
					fail("Tab held: no scoreboard");
				}
				return;
			}
			if (!shot_taken_) {
				shot_taken_ = true;
				pass("Tab held: the scoreboard shows", p_t);
				mark_s_ = p_t;
				return;
			}
			if (p_t - mark_s_ < kOverlayCheckRowsSeconds) {
				return;
			}
			shot("overlay_scoreboard", p_t, p_io);
			p_io.press(godot::KEY_TAB, false);
			stage_ = Stage::TabUp;
			mark_s_ = p_t;
			return;
		case Stage::TabUp:
			if (scoreboard(r_screens)) {
				if (p_t - mark_s_ > kOverlayCheckReactSeconds) {
					fail("Tab released: the scoreboard still shows");
				}
				return;
			}
			pass("Tab released: the scoreboard hides", p_t);
			stage_ = Stage::Done;
			return;
		case Stage::Done:
			return;
	}
}

std::string OverlayCheck::report() const {
	std::string out = "[ui] overlay check:\n";
	for (const std::string &l : lines_) {
		out += "[ui]   " + l + "\n";
	}
	for (const std::string &s : shots_) {
		out += "[ui]   shot " + s + "\n";
	}
	out += passed_ ? "OVERLAYS: PASS" : "OVERLAYS: FAIL " + failure_;
	return out;
}

}
