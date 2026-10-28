#include "eor_check.h"

#include "eor_camera.h"
#include "eor_flash.h"
#include "native_eor.h"
#include "native_screens.h"

#include "bf3/host/ui_flow_session.h"

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/ui_input.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/global_constants.hpp>

#include <algorithm>
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

std::string fixed_text(double p_s) {
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

double EorCheck::page_after_banner(const EorCheckIo &p_io) {
	return double(p_io.eor.pre_eor_s) - double(p_io.eor.fade_s);
}

bool EorCheck::timed(double p_took, double p_want, double p_tolerance, const char *p_what) {
	note(std::string(p_what) + " " + fixed_text(p_took) + " s (install " + fixed_text(p_want) + " s)");
	if (std::fabs(p_took - p_want) > p_tolerance) {
		fail(std::string(p_what) + " after " + fixed_text(p_took) + " s, the install's timeline says " +
				fixed_text(p_want) + " s");
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

bool EorCheck::check_page(Bf3UiScreens &r_screens, const ui::HudModel &p_model, double p_left_s) {
	const std::string header = text_at(
			r_screens, kEorScoreboardPartition, std::string(ui::kHudScreenInstance) + ".EorHeader_01.headerText");
	const uint8_t local = p_model.local_player;
	const int8_t roster_team = local < p_model.players.size() ? p_model.players[local].team : int8_t(0);
	const bool won = roster_team != 0 && roster_team == p_model.end_of_round.winner;
	const std::string want = won ? "YOUR TEAM WON!" : "YOUR TEAM LOST!";
	const ui::HudPlayerList &friendly = p_model.team.friendly_team_players;
	const bool local_friendly = std::find(friendly.index.begin(), friendly.index.begin() + friendly.count, local) !=
			friendly.index.begin() + friendly.count;
	const std::string caption = text_field(r_screens, "TextField_02");
	const std::string value = text_field(r_screens, "TextField_01");
	const long seconds = std::strtol(value.c_str(), nullptr, 10);
	note("EOR page: header '" + header + "', '" + caption + "' '" + value + "', own roster team " +
			std::to_string(int(roster_team)) + ", winner " + std::to_string(int(p_model.end_of_round.winner)) +
			", team rows " + std::to_string(friendly.count) + " + " +
			std::to_string(p_model.team.enemy_team1_players.count));
	if (header != want) {
		fail("EorHeader reads '" + header + "', the own roster team against the winner gives '" + want + "'");
		return false;
	}
	if (!local_friendly) {
		fail("the own row is not in the friendly list (List_01)");
		return false;
	}
	const long lowest = long(std::floor(p_left_s - kEorCheckToleranceS));
	const long highest = long(std::floor(p_left_s + kEorCheckToleranceS));
	if (caption != "NEXT ROUND STARTS IN:" || value.empty() || seconds < lowest || seconds > highest) {
		fail("countdown reads '" + caption + "' '" + value + "', want NEXT ROUND STARTS IN: and " +
				std::to_string(lowest) + ".." + std::to_string(highest) + " (EorTime less the time on the page)");
		return false;
	}
	if (!visible(r_screens, kEorTwoTeamsPartition) || p_model.team.friendly_team_players.count == 0) {
		fail("the two-team scoreboard is not up with rows");
		return false;
	}
	return true;
}

bool EorCheck::check_eor_view(Bf3UiScreens &r_screens, const EorCheckIo &p_io) {
	if (visible(r_screens, kEorHud64Partition) || visible(r_screens, ui::kEorWinningTeamScreenPartition)) {
		fail("hud64 or the banner is still up under the END OF ROUND page");
		return false;
	}
	note("hud64 and the banner are closed under the page");
	if (!p_io.camera_data || p_io.eor_camera == nullptr || p_io.camera == nullptr || p_io.flash == nullptr) {
		fail("no EOR camera in the level data, or no camera or flash to check");
		return false;
	}
	const godot::Vector3 at = p_io.camera->get_global_transform().origin;
	const bf3::core::level::Vec3 &want = p_io.camera_data->transform.trans;
	const double off = double((at - godot::Vector3(want[0], want[1], want[2])).length());
	const double fov = p_io.camera->get_fov();
	note("camera at " + fixed_text(at.x) + ", " + fixed_text(at.y) + ", " + fixed_text(at.z) + " fov " +
			fixed_text(fov) + "; the level's EOR camera (ShowTopPlayers) at " + fixed_text(want[0]) + ", " +
			fixed_text(want[1]) + ", " + fixed_text(want[2]) + " fov " + fixed_text(p_io.camera_data->fov_deg) + ", " +
			fixed_text(off) + " m off");
	if (!p_io.eor_camera->active() || off > kEorCheckCameraM ||
			std::abs(fov - double(p_io.camera_data->fov_deg)) > kEorCheckFovDeg) {
		fail("the camera is not the level's EOR camera during the END OF ROUND page");
		return false;
	}
	const EorFlashStats &f = p_io.flash->stats();
	note("flash: " + std::to_string(f.round_overs) + " round over, " + std::to_string(f.white_frames) +
			" white frames, peak visibility " + fixed_text(f.peak_visibility) + ", " + std::to_string(f.black_frames) +
			" black frames, " + std::to_string(f.fade_in_frames) + " fade-in frames, UI faded on " +
			std::to_string(f.ui_faded_frames) + " frames to " + fixed_text(f.peak_ui_faded));
	if (f.round_overs < 1 || f.white_frames == 0 || f.peak_visibility < kEorCheckFlashPeak || f.black_frames == 0 ||
			f.fade_in_frames == 0 || f.peak_ui_faded < 1.0f) {
		fail("mp_eor_fading's white flash, UI fade and screen fade and EndOfRound's fade-in did not all play");
		return false;
	}
	return true;
}

bool EorCheck::check_player_view(const EorCheckIo &p_io) {
	if (!p_io.camera_data || p_io.eor_camera == nullptr || p_io.camera == nullptr) {
		fail("no EOR camera or camera to check after the restart");
		return false;
	}
	const godot::Vector3 at = p_io.camera->get_global_transform().origin;
	const bf3::core::level::Vec3 &eor = p_io.camera_data->transform.trans;
	const double off = double((at - godot::Vector3(eor[0], eor[1], eor[2])).length());
	note("after the restart the camera is at " + fixed_text(at.x) + ", " + fixed_text(at.y) + ", " + fixed_text(at.z) +
			" fov " + fixed_text(p_io.camera->get_fov()) + ", " + fixed_text(off) +
			" m from the EOR camera, released " + std::to_string(p_io.eor_camera->stats().released) + " time(s)");
	if (p_io.eor_camera->active() || p_io.eor_camera->stats().released < 1 || off <= kEorCheckCameraM) {
		fail("the player's camera did not come back at the in-place restart");
		return false;
	}
	return true;
}

bool EorCheck::click_popup_ok(Bf3UiScreens &r_screens) {
	ui::HudScreen *s = r_screens.hud_screen(kEorPopupPartition);
	if (s == nullptr) {
		fail("popupgeneric has no screen");
		return false;
	}
	const std::string root = std::string(ui::kHudScreenInstance);
	const std::string title = text_at(r_screens, kEorPopupPartition, root + ".PopupGenericWidget_01.popupTitle");
	const std::string text = text_at(r_screens, kEorPopupPartition, root + ".PopupGenericWidget_01.popupText");
	const std::string ok_path = root + ".PopupButtonsManager_01.holder.btn0";
	const std::string ok = text_at(r_screens, kEorPopupPartition, ok_path + ".button.label.txt");
	const std::string cancel =
			text_at(r_screens, kEorPopupPartition, root + ".PopupButtonsManager_01.holder.btn1.button.label.txt");
	note("QuitPopup reads '" + title + "' / '" + text + "' / '" + ok + "' '" + cancel + "'");
	if (title != kEorQuitTitle || text != kEorQuitText || ok != kEorQuitOk || cancel != kEorQuitCancel) {
		fail(std::string("the QuitPopup does not read '") + kEorQuitTitle + "' / '" + kEorQuitText + "' / '" +
				kEorQuitOk + "' '" + kEorQuitCancel + "'");
		return false;
	}
	const ui::As2Clip button = s->clip(ok_path);
	const ui::SwfSceneNode *node = button.node();
	if (node == nullptr || !node->world_visible) {
		fail("the QuitPopup's OK button is not drawn");
		return false;
	}
	const ui::SwfRect at = ui::swf_transform_bounds(button.local_bounds(), node->world);
	const float x = (at.x_min + at.x_max) * 0.5f;
	const float y = (at.y_min + at.y_max) * 0.5f;
	const std::string hit = s->hit_target(x, y);
	note("OK at " + fixed_text(x) + ", " + fixed_text(y) + " hits '" + hit + "'");
	if (hit != ok_path) {
		fail("the OK button's centre hits '" + hit + "'");
		return false;
	}
	click(r_screens, godot::Vector2(x, y));
	note("clicked OK");
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
			if (!flash_shot_ && p_io.flash != nullptr && p_io.flash->stats().last_visibility >= 0.5f) {
				flash_shot_ = true;
				shot(p_io, "eor_flash");
			}
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
			if (!banner_under_hud64_checked_ && visible(r_screens, kEorHud64Partition) &&
					p_now_s - step_s_ >= kEorCheckSettleS) {
				banner_under_hud64_checked_ = true;
				if (banner) {
					fail("eorwinningteamscreen is still up under hud64; 18.57 at 327.2 s drops it on the frame hud64 "
						 "opens");
					break;
				}
				note("eorwinningteamscreen closed when hud64 opened");
			}
			if (page) {
				if (!banner_under_hud64_checked_) {
					fail("the END OF ROUND page opened before the banner was checked under hud64");
					break;
				}
				page_s_ = p_now_s;
				if (timed(p_now_s - banner_s_, page_after_banner(p_io), kEorCheckToleranceS, "EOR page")) {
					go(Step::PageSettled, p_now_s);
				}
			}
			break;
		case Step::PageSettled:
			if (p_now_s - step_s_ >= kEorCheckSettleS) {
				if (check_eor_view(r_screens, p_io) &&
						check_page(r_screens, p_model, double(p_io.eor.eor_s) - (p_now_s - page_s_))) {
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
				fail("eordetailsstatsscreen not up " + fixed_text(kEorCheckStepWaitS) + " s after the STATS click");
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
				if (timed(p_now_s - page_s_, double(p_io.eor.eor_s), kEorCheckToleranceS, "next round")) {
					go(Step::Banner2, p_now_s);
				}
			}
			break;
		case Step::Banner2:
			deploy(r_screens, p_now_s, p_io);
			if (!player_view_checked_ && p_now_s - step_s_ >= kEorCheckSettleS) {
				player_view_checked_ = true;
				if (!check_player_view(p_io)) {
					break;
				}
			}
			round2_viewmodel_frames_ += p_io.own_viewmodel_presented ? 1u : 0u;
			if (banner) {
				banner_s_ = p_now_s;
				note("second round over; the own viewmodel was presented on " +
						std::to_string(round2_viewmodel_frames_) + " frames of the second round");
				if (round2_viewmodel_frames_ == 0) {
					fail("the own viewmodel was never presented in the second round");
					break;
				}
				go(Step::Page2, p_now_s);
			}
			break;
		case Step::Page2:
			if (page && page_s_ < banner_s_) {
				page_s_ = p_now_s;
				if (!timed(p_now_s - banner_s_, page_after_banner(p_io), kEorCheckToleranceS, "second EOR page")) {
					break;
				}
			}
			if (page && p_now_s - page_s_ >= kEorCheckSettleS) {
				if (!check_page(r_screens, p_model, double(p_io.eor.eor_s) - (p_now_s - page_s_))) {
					break;
				}
				click(r_screens, godot::Vector2(kEorCheckQuitX, kEorCheckQuitY));
				note("clicked QUIT");
				go(Step::QuitClicked, p_now_s);
			}
			break;
		case Step::QuitClicked:
			if (visible(r_screens, kEorPopupPartition)) {
				note("QuitPopup up");
				shot_later("eor_quit_popup", p_now_s + kEorCheckPopupShotS);
				go(Step::PopupShown, p_now_s);
			} else if (p_now_s - step_s_ > kEorCheckStepWaitS) {
				fail("popupgeneric not up after the QUIT click");
			}
			break;
		case Step::PopupShown:
			if (shot_name_ == nullptr && click_popup_ok(r_screens)) {
				go(Step::PopupActivated, p_now_s);
			}
			break;
		case Step::PopupActivated:
			if (p_eor.quit_requested()) {
				note("OK: QuitGame requested");
				passed_ = true;
				done_ = true;
				step_ = Step::Done;
			} else if (p_now_s - step_s_ > kEorCheckStepWaitS) {
				fail("the click on the QuitPopup's OK did not quit");
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
