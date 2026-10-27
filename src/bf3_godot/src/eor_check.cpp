#include "eor_check.h"

#include "native_eor.h"
#include "native_screens.h"

#include "bf3/host/ui_flow_session.h"

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/ui_input.h"

#include <godot_cpp/classes/global_constants.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

namespace {

bool visible(const Bf3UiScreens &p_screens, std::string_view p_partition) {
	const std::optional<UiScreenRecord> r = p_screens.screen_record(p_partition);
	return r && r->visible;
}

std::string text_at(Bf3UiScreens &r_screens, std::string_view p_partition, const std::string &p_path) {
	ui::HudScreen *s = r_screens.hud_screen(p_partition);
	return s != nullptr ? s->clip(p_path).text() : std::string();
}

std::string text_field(Bf3UiScreens &r_screens, std::string_view p_widget) {
	ui::HudScreen *s = r_screens.hud_screen(kEorTabsPartition);
	const std::string path = std::string(ui::kHudScreenInstance) + "." + std::string(p_widget);
	ui::HudWidgetHost *h = s != nullptr ? s->host(path) : nullptr;
	const ui::As2Value *row_type = h != nullptr ? h->construct_param("m_rowType") : nullptr;
	if (row_type == nullptr) {
		return std::string();
	}
	return s->clip(path + "." + row_type->to_string() + ".txtDisplay").text();
}

std::string seconds_text(double p_s) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f", p_s);
	return text;
}

}

void EorCheck::note(std::string p_line) {
	if (lines_.size() < kEorCheckMaxLines) {
		lines_.push_back(std::move(p_line));
	}
}

void EorCheck::collect(Bf3UiScreens &r_screens, bf3::host::UiFlowSession *p_session) {
	if (p_session != nullptr) {
		for (const std::string &l : p_session->log()) {
			if (l.find("ndOfRound") != std::string::npos || l.find("eor") != std::string::npos ||
					l.find("popup") != std::string::npos || l.find("Quit") != std::string::npos) {
				note("flow " + l);
			}
		}
		p_session->clear_log();
	}
	for (const std::string &l : r_screens.log()) {
		if (l.find("eor") != std::string::npos || l.find("scoreboard") != std::string::npos ||
				l.find("popup") != std::string::npos) {
			note("screens " + l);
		}
	}
	r_screens.clear_log();
}

void EorCheck::deploy(const Bf3UiScreens &p_screens, double p_now_s, const EorCheckIo &p_io) {
	if (!p_io.press || !visible(p_screens, ui::kSpawnScreenPcPartition) ||
			p_now_s - deploy_s_ < kEorCheckDeployEveryS) {
		return;
	}
	deploy_s_ = p_now_s;
	p_io.press(godot::KEY_ENTER, true);
	p_io.press(godot::KEY_ENTER, false);
}

void EorCheck::fail(std::string p_why) {
	if (failure_.empty()) {
		failure_ = std::move(p_why);
	}
	done_ = true;
}

void EorCheck::go(Step p_step, double p_now_s) {
	step_ = p_step;
	step_s_ = p_now_s;
}

bool EorCheck::timed(double p_took, double p_want, double p_tolerance, const char *p_what) {
	note(std::string(p_what) + " " + seconds_text(p_took) + " s (install " + seconds_text(p_want) + " s)");
	if (std::fabs(p_took - p_want) > p_tolerance) {
		fail(std::string(p_what) + " after " + seconds_text(p_took) + " s, the install's timeline says " +
				seconds_text(p_want) + " s");
		return false;
	}
	return true;
}

void EorCheck::shot(const EorCheckIo &p_io, const char *p_name) {
	if (shot_dir_.empty() || !p_io.shot) {
		return;
	}
	const std::string path = shot_dir_ + "/" + p_name + ".png";
	note(std::string("shot ") + path + (p_io.shot(path) ? "" : " FAILED"));
}

void EorCheck::shot_later(const char *p_name, double p_at_s) {
	shot_name_ = p_name;
	shot_at_s_ = p_at_s;
}

void EorCheck::click(Bf3UiScreens &r_screens, godot::Vector2 p_at) {
	r_screens.mouse_move(p_at);
	r_screens.mouse_button(p_at, int32_t(godot::MOUSE_BUTTON_LEFT), true);
	r_screens.mouse_button(p_at, int32_t(godot::MOUSE_BUTTON_LEFT), false);
}

void EorCheck::press_concept(Bf3UiScreens &r_screens, int32_t p_concept) {
	r_screens.input_concept(p_concept, true);
	r_screens.input_concept(p_concept, false);
}

bool EorCheck::check_page(Bf3UiScreens &r_screens, const ui::HudModel &p_model) {
	const std::string header = text_at(
			r_screens, kEorScoreboardPartition, std::string(ui::kHudScreenInstance) + ".EorHeader_01.headerText");
	const std::string want = p_model.end_of_round.local_player_in_winning_team ? "YOUR TEAM WON!" : "YOUR TEAM LOST!";
	const std::string caption = text_field(r_screens, "TextField_02");
	const std::string value = text_field(r_screens, "TextField_01");
	const long seconds = std::strtol(value.c_str(), nullptr, 10);
	note("EOR page: header '" + header + "', '" + caption + "' '" + value + "', team rows " +
			std::to_string(p_model.team.friendly_team_players.count) + " + " +
			std::to_string(p_model.team.enemy_team1_players.count));
	if (header != want) {
		fail("EorHeader reads '" + header + "', the round's result is '" + want + "'");
		return false;
	}
	if (caption != "NEXT ROUND STARTS IN:" || value.empty() || seconds < 44 || seconds > 48) {
		fail("countdown reads '" + caption + "' '" + value + "', want NEXT ROUND STARTS IN: and 44..48");
		return false;
	}
	if (!visible(r_screens, kEorTwoTeamsPartition) || p_model.team.friendly_team_players.count == 0) {
		fail("the two-team scoreboard is not up with rows");
		return false;
	}
	return true;
}

bool EorCheck::frame(Bf3UiScreens &r_screens, bf3::host::UiFlowSession *p_session, const NativeEor &p_eor,
		const ui::HudModel &p_model, double p_now_s, const EorCheckIo &p_io) {
	if (done_) {
		return true;
	}
	collect(r_screens, p_session);
	if (shot_name_ != nullptr && p_now_s >= shot_at_s_) {
		shot(p_io, shot_name_);
		shot_name_ = nullptr;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
		step_s_ = p_now_s;
	}
	if (p_now_s - start_s_ > kEorCheckSeconds) {
		fail("timed out at step " + std::to_string(int(step_)));
		return true;
	}
	const bool banner = visible(r_screens, ui::kEorWinningTeamScreenPartition);
	const bool page = visible(r_screens, kEorTabsPartition) && visible(r_screens, kEorScoreboardPartition);
	switch (step_) {
		case Step::Banner:
			deploy(r_screens, p_now_s, p_io);
			if (over_s_ < 0.0 && p_model.end_of_round.visible) {
				over_s_ = p_now_s;
				note("round over in the HUD model");
			} else if (over_s_ >= 0.0 && !p_model.end_of_round.visible) {
				over_s_ = -1.0;
				note("next round without the banner (the own soldier was not spawned)");
			}
			if (banner) {
				banner_s_ = p_now_s;
				note("banner eorwinningteamscreen up");
				shot_later("eor_banner", p_now_s + kEorCheckBannerShotS);
				go(Step::Scoreboard, p_now_s);
			}
			break;
		case Step::Scoreboard:
			if (visible(r_screens, kEorHud64Partition)) {
				shot_later("eor_hud64", p_now_s + kEorCheckHud64ShotS);
				if (timed(p_now_s - banner_s_, double(p_io.eor.scoreboard_delay_s), kEorCheckToleranceS,
							"hud64 scoreboard")) {
					go(Step::Page, p_now_s);
				}
			}
			break;
		case Step::Page:
			if (page) {
				page_s_ = p_now_s;
				if (timed(p_now_s - banner_s_, double(p_io.eor.pre_eor_s), kEorCheckToleranceS, "EOR page")) {
					go(Step::PageSettled, p_now_s);
				}
			}
			break;
		case Step::PageSettled:
			if (p_now_s - step_s_ >= kEorCheckSettleS) {
				if (check_page(r_screens, p_model)) {
					shot(p_io, "eor_scoreboard");
					click(r_screens, godot::Vector2(kEorCheckStatsTabX, kEorCheckStatsTabY));
					note("clicked STATS");
					go(Step::StatsClicked, p_now_s);
				}
			}
			break;
		case Step::StatsClicked:
			if (visible(r_screens, kEorStatsPartition)) {
				note("STATS page up after the click");
				shot_later("eor_stats", p_now_s + kEorCheckStatsShotS);
				go(Step::StatsShown, p_now_s);
			} else if (p_now_s - step_s_ > kEorCheckStepWaitS) {
				fail("eordetailsstatsscreen not up " + seconds_text(kEorCheckStepWaitS) + " s after the STATS click");
			}
			break;
		case Step::StatsShown:
			if (shot_name_ == nullptr) {
				press_concept(r_screens, int32_t(ui::UiInputConcept::TabLeft));
				go(Step::TabLeft, p_now_s);
			}
			break;
		case Step::TabLeft:
			if (visible(r_screens, kEorScoreboardPartition) && !visible(r_screens, kEorStatsPartition)) {
				note("TabLeft: SCOREBOARD page up");
				press_concept(r_screens, int32_t(ui::UiInputConcept::TabRight));
				go(Step::TabRight, p_now_s);
			} else if (p_now_s - step_s_ > kEorCheckStepWaitS) {
				fail("TabLeft did not bring the scoreboard page back");
			}
			break;
		case Step::TabRight:
			if (visible(r_screens, kEorStatsPartition)) {
				note("TabRight: STATS page up");
				go(Step::Restart, p_now_s);
			} else if (p_now_s - step_s_ > kEorCheckStepWaitS) {
				fail("TabRight did not bring the STATS page");
			}
			break;
		case Step::Restart:
			if (!visible(r_screens, kEorTabsPartition)) {
				const double next =
						double(p_io.eor.restart_delay_s) - double(p_io.eor.fade_s) - double(p_io.eor.pre_eor_s);
				if (timed(p_now_s - page_s_, next, kEorCheckToleranceS, "next round")) {
					go(Step::Banner2, p_now_s);
				}
			}
			break;
		case Step::Banner2:
			deploy(r_screens, p_now_s, p_io);
			if (banner) {
				banner_s_ = p_now_s;
				note("second round over");
				go(Step::Page2, p_now_s);
			}
			break;
		case Step::Page2:
			if (page && p_now_s - banner_s_ >= double(p_io.eor.pre_eor_s) + kEorCheckSettleS) {
				click(r_screens, godot::Vector2(kEorCheckQuitX, kEorCheckQuitY));
				note("clicked QUIT");
				go(Step::QuitClicked, p_now_s);
			}
			break;
		case Step::QuitClicked:
			if (visible(r_screens, kEorPopupPartition)) {
				note("QuitPopup up");
				shot(p_io, "eor_quit_popup");
				press_concept(r_screens, int32_t(ui::UiInputConcept::Activate));
				go(Step::PopupActivated, p_now_s);
			} else if (p_now_s - step_s_ > kEorCheckStepWaitS) {
				fail("popupgeneric not up after the QUIT click");
			}
			break;
		case Step::PopupActivated:
			if (p_eor.quit_requested()) {
				note("OK: QuitGame requested");
				passed_ = true;
				done_ = true;
				step_ = Step::Done;
			} else if (p_now_s - step_s_ > kEorCheckStepWaitS) {
				fail("Activate on the QuitPopup did not quit");
			}
			break;
		case Step::Done:
			break;
	}
	return done_;
}

std::string EorCheck::report() const {
	std::string out;
	for (const std::string &l : lines_) {
		out += "[eor] " + l + "\n";
	}
	out += std::string("EOR CHECK: ") + (passed_ ? "PASS" : "FAIL " + failure_);
	return out;
}

}
