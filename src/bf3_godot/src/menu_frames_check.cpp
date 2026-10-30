#include "menu_frames_check.h"

#include "deploy_check.h"
#include "deploy_screens.h"
#include "level_session_menu.h"
#include "native_flow.h"
#include "native_screens.h"
#include "swf_view.h"
#include "video_memory.h"

#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/percentile.h"

#include "bf3/host/ui_flow_session.h"

#include <algorithm>
#include <numeric>
#include <utility>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

namespace {

std::string options_tab_button(size_t p_tab) {
	return std::string(ui::kHudScreenInstance) + ".TabBar_01.tabBarHolder.button_" + std::to_string(p_tab);
}

}

MenuFramesCheck::MenuFramesCheck(std::string p_shot_dir) : shot_dir_(std::move(p_shot_dir)) {
	frames_.reserve(kMenuFramesMaxFrames);
}

const char *MenuFramesCheck::phase_name(Phase p_phase) {
	switch (p_phase) {
		case Phase::Setup:
			return "setup";
		case Phase::First:
			return "first visits";
		case Phase::Deploy:
			return "deploy screen";
		case Phase::Customize:
			return "CUSTOMIZE";
		case Phase::Play:
			return "play";
		case Phase::Menu:
			return "Escape menu";
		case Phase::Options:
			return "Options";
	}
	return "?";
}

bool MenuFramesCheck::gated(Phase p_phase) {
	return p_phase == Phase::Deploy || p_phase == Phase::Customize || p_phase == Phase::Menu ||
			p_phase == Phase::Options;
}

bool MenuFramesCheck::counted(const MenuFrameSample &p_f) {
	return !p_f.shot && !p_f.recovering;
}

bool MenuFramesCheck::shown(const Ctx &p_c, std::string_view p_partition) {
	const UiScreenSlot s = p_c.screens.find_screen(p_partition);
	return s.screen != nullptr && s.shown;
}

void MenuFramesCheck::tap(const Ctx &p_c, godot::Key p_key) {
	p_c.io.press(p_key, true);
	p_c.io.press(p_key, false);
}

void MenuFramesCheck::click_clip(const Ctx &p_c, std::string_view p_partition, const std::string &p_path) {
	const std::optional<godot::Vector2> at = p_c.screens.clip_centre(p_partition, p_path);
	if (!at) {
		failure_ = p_path + " is not drawn in " + ui::ui_data_category_leaf(p_partition) + "; screens:";
		for (size_t i = 0; i < p_c.screens.screen_count(); ++i) {
			const UiScreenSlot s = p_c.screens.screen_slot(i);
			failure_ += bf3::core::fmt(" %s (shown %d, view %s, node %d)",
					ui::ui_data_category_leaf(s.partition).c_str(), int(s.shown),
					s.view == nullptr ? "none" : (s.view->is_visible_in_tree() ? "visible" : "hidden"),
					int(s.screen != nullptr && s.screen->scene().find_node(p_path) != nullptr));
		}
		return;
	}
	p_c.io.move(*at);
	p_c.io.click(*at, true);
	p_c.io.click(*at, false);
}

void MenuFramesCheck::options_round(std::vector<Step> &r_steps, Phase p_phase, bool p_leave) {
	for (size_t t = 0; t < ui::kOptionsTabCount; ++t) {
		const std::string_view from = ui::kOptionsScreens[t];
		const size_t to = (t + 1) % ui::kOptionsTabCount;
		Act act = [from, to](MenuFramesCheck &r_self, const Ctx &c) {
			r_self.click_clip(c, from, options_tab_button(to));
		};
		if (p_leave && to == 0) {
			act = [](MenuFramesCheck &, const Ctx &c) { tap(c, godot::Key::KEY_ESCAPE); };
		}
		r_steps.push_back({ std::string("Options ") + std::string(ui::options_tab_name(ui::OptionsTab(t))), p_phase,
				kMenuFramesHoldSeconds,
				[from, to](const Ctx &c) {
					return shown(c, from) && c.screens.clip_centre(from, options_tab_button(to)).has_value();
				},
				std::move(act) });
	}
}

std::vector<MenuFramesCheck::Step> MenuFramesCheck::steps() {
	using K = godot::Key;
	const auto press = [](K p_key) { return Act([p_key](MenuFramesCheck &, const Ctx &c) { tap(c, p_key); }); };
	const auto any = [](const Ctx &) { return true; };
	const Act none = [](MenuFramesCheck &, const Ctx &) {};
	const auto spawn_screen = [](const Ctx &c) {
		return shown(c, kSpawnScreenPcPartition) && shown(c, kSpawnButtonScreenPartition) &&
				!shown(c, kCustomizeScreenPartitions[0]);
	};
	const auto customize_open = [](const Ctx &c) { return shown(c, kCustomizeScreenPartitions[0]); };
	const Act customize = [](MenuFramesCheck &r_self, const Ctx &c) {
		r_self.click_clip(c, kSpawnScreenPcPartition, std::string(kDeployCheckTabs[0].button));
	};
	const auto deployed = [](const Ctx &c) {
		return c.io.alive && c.io.game_input && !shown(c, kSpawnScreenPcPartition);
	};
	const auto menu_open = [](const Ctx &c) {
		return shown(c, kIngameMenuPartition) && c.menu.in_menu() && !c.io.game_input &&
				!shown(c, kPopupGenericPartition);
	};
	const auto in_play = [](const Ctx &c) {
		return !shown(c, kIngameMenuPartition) && !c.menu.in_menu() && c.io.game_input && c.io.alive;
	};
	std::vector<Step> s;
	s.push_back({ "spawn screen up", Phase::Setup, 2.0, spawn_screen, none });
	s.push_back({ "first deploy screen", Phase::First, kMenuFramesHoldSeconds, any, customize });
	s.push_back({ "first CUSTOMIZE", Phase::First, kMenuFramesHoldSeconds, customize_open, press(K::KEY_ESCAPE) });
	s.push_back({ "first CUSTOMIZE closed", Phase::Setup, kMenuFramesHoldSeconds, spawn_screen, none });
	s.push_back({ "deploy screen idle", Phase::Deploy, 3.0, any, customize });
	for (uint32_t i = 0; i < 2; ++i) {
		s.push_back(
				{ "CUSTOMIZE opens", Phase::Customize, kMenuFramesHoldSeconds, customize_open, press(K::KEY_ESCAPE) });
		s.push_back({ "Escape closes CUSTOMIZE", Phase::Customize, kMenuFramesHoldSeconds, spawn_screen,
				i == 0 ? customize : press(K::KEY_ENTER) });
	}
	s.push_back({ "deployed", Phase::Setup, 2.0, deployed, none, true, -1.0, true, false });
	s.push_back({ "play, no menu", Phase::Play, kMenuFramesBaselineSeconds, any, press(K::KEY_ESCAPE) });
	s.push_back({ "first menu", Phase::First, kMenuFramesHoldSeconds, menu_open, press(K::KEY_UP), true, -1.0, true,
			true });
	s.push_back({ "first menu, Up to QUIT", Phase::First, kMenuFramesKeySeconds, any, press(K::KEY_UP) });
	s.push_back({ "first menu, Up to OPTIONS", Phase::First, kMenuFramesKeySeconds, any, press(K::KEY_ENTER) });
	options_round(s, Phase::First, true);
	s.push_back({ "first Options left", Phase::First, kMenuFramesHoldSeconds, menu_open, press(K::KEY_ESCAPE) });
	s.push_back({ "first menu closed", Phase::First, kMenuFramesHoldSeconds, in_play, press(K::KEY_ESCAPE), true });
	for (uint32_t i = 0; i < kMenuFramesMenuCycles; ++i) {
		s.push_back({ "Escape opens the menu", Phase::Menu, kMenuFramesHoldSeconds, menu_open, press(K::KEY_ESCAPE),
				true, -1.0, true, true });
		s.push_back(
				{ "Escape closes the menu", Phase::Menu, kMenuFramesHoldSeconds, in_play, press(K::KEY_ESCAPE), true });
	}
	s.push_back({ "the menu for OPTIONS", Phase::Menu, kMenuFramesKeySeconds, menu_open, press(K::KEY_UP), true, -1.0,
			true, true });
	s.push_back({ "Up to QUIT", Phase::Menu, kMenuFramesKeySeconds, any, press(K::KEY_UP) });
	s.push_back({ "Up to OPTIONS", Phase::Menu, kMenuFramesKeySeconds, any, press(K::KEY_ENTER) });
	for (uint32_t r = 0; r < kMenuFramesOptionsRounds; ++r) {
		options_round(s, Phase::Options, r + 1 == kMenuFramesOptionsRounds);
	}
	s.push_back({ "Escape leaves Options for the menu", Phase::Menu, kMenuFramesHoldSeconds, menu_open,
			press(K::KEY_ESCAPE) });
	s.push_back({ "Escape returns to play", Phase::Menu, kMenuFramesHoldSeconds, in_play, none });
	return s;
}

void MenuFramesCheck::record(const Ctx &p_c) {
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	const uint64_t opened = p_c.screens.stats().opened;
	MenuFrameSample f;
	f.step = uint16_t(std::min(next_, steps_.size() - 1));
	f.wall_ms = last_frame_ ? float(std::chrono::duration<double, std::milli>(now - *last_frame_).count()) : 0.0f;
	f.session_ms = float(p_c.io.session_ms);
	f.ui_ms = float(p_c.io.ui_ms);
	f.views_ms = views_seen_s_ < 0.0 ? 0.0f : float((p_c.io.views_seconds_total - views_seen_s_) * 1000.0);
	views_seen_s_ = p_c.io.views_seconds_total;
	f.setup_ms = float(p_c.io.setup_ms);
	f.render_cpu_ms = float(p_c.io.render_cpu_ms);
	f.render_gpu_ms = float(p_c.io.render_gpu_ms);
	f.opened = uint16_t(std::min<uint64_t>(opened - opened_seen_, 0xFFFF));
	f.glitches = uint16_t(std::min<uint64_t>(p_c.io.glitch_triggers - glitches_seen_, 0xFFFF));
	f.glitch_drawn = p_c.io.glitch_drawn;
	f.shot = shot_last_;
	f.recovering = recovering_;
	recovery_frames_ += recovering_ ? 1u : 0u;
	shot_last_ = false;
	opened_seen_ = opened;
	glitches_seen_ = p_c.io.glitch_triggers;
	glitch_frames_ += f.glitch_drawn ? 1u : 0u;
	video_peak_mb_ = std::max(video_peak_mb_, p_c.io.video_mb);
	if (steps_[f.step].phase == Phase::Play && video_play_mb_ == 0.0) {
		video_play_mb_ = p_c.io.video_mb;
	}
	if (p_c.io.local_video_bytes) {
		local_peak_bytes_ = std::max(local_peak_bytes_, *p_c.io.local_video_bytes);
		if (steps_[f.step].phase == Phase::Play && !local_play_bytes_) {
			local_play_bytes_ = p_c.io.local_video_bytes;
		}
	}
	if (!shot_dir_.empty() && p_c.io.shot && (f.glitch_drawn || f.glitches != 0) && !gated(steps_[f.step].phase) &&
			shots_.size() < kMenuFramesShots && step_shots_[f.step] < kMenuFramesShotsPerStep) {
		++step_shots_[f.step];
		const std::string path = shot_dir_ + "/glitch_" + std::to_string(shots_.size()) + ".png";
		shots_.push_back(path + (p_c.io.shot(path) ? "" : " FAILED") + " (" + steps_[f.step].name + ")");
		shot_last_ = true;
	}
	last_frame_ = std::chrono::steady_clock::now();
	if (frames_.size() < kMenuFramesMaxFrames && f.wall_ms > 0.0f) {
		frames_.push_back(f);
	}
}

std::vector<double> MenuFramesCheck::walls(bool p_menus) const {
	std::vector<double> out;
	for (const MenuFrameSample &f : frames_) {
		const Phase ph = steps_[f.step].phase;
		if (counted(f) && (p_menus ? gated(ph) : ph == Phase::Play)) {
			out.push_back(f.wall_ms);
		}
	}
	return out;
}

void MenuFramesCheck::judge() {
	const std::vector<double> base = walls(false);
	const std::vector<double> menus = walls(true);
	if (base.empty() || menus.empty()) {
		failure_ = "no frames recorded in play or in the menus";
		return;
	}
	const double base_median = bf3::core::interpolated_percentile(base, 0.5);
	const double menus_median = bf3::core::interpolated_percentile(menus, 0.5);
	const double hitch_ms = kMenuFramesHitchFactor * base_median;
	const double stall_ms = std::max(hitch_ms, kMenuFramesGateRefreshes * kMenuFramesRefreshMs);
	const double median_budget = base_median * kMenuFramesMedianFactor + kMenuFramesMedianSlackMs;
	size_t hitches = 0;
	size_t stalls = 0;
	double worst = 0.0;
	for (const MenuFrameSample &f : frames_) {
		if (gated(steps_[f.step].phase) && counted(f)) {
			hitches += f.wall_ms > hitch_ms ? 1u : 0u;
			stalls += f.wall_ms > stall_ms ? 1u : 0u;
			worst = std::max(worst, double(f.wall_ms));
		}
	}
	verdict_ = bf3::core::fmt("no-menu median %.2f ms, menus median %.2f ms (budget %.2f); hitches over 3 x the "
							  "no-menu median (%.1f ms): %zu, worst %.2f ms; stalls over %.1f ms (gated): %zu",
			base_median, menus_median, median_budget, hitch_ms, hitches, worst, stall_ms, stalls);
	if (stalls != 0) {
		failure_ = std::to_string(stalls) + " stall(s): " + verdict_;
	} else if (menus_median > median_budget) {
		failure_ = "the menus' median frame is over budget: " + verdict_;
	} else if (glitches_seen_ == 0 || glitch_frames_ == 0) {
		failure_ = "no glitch was drawn";
	}
}

size_t MenuFramesCheck::resume_step() const {
	for (size_t i = std::min(next_, steps_.size() - 1) + 1; i-- > 0;) {
		if (steps_[i].resume) {
			return i;
		}
	}
	return 0;
}

void MenuFramesCheck::recover(const Ctx &p_c, double p_now_s) {
	const bool menu_up = shown(p_c, kIngameMenuPartition) || shown(p_c, kPopupGenericPartition) ||
			std::any_of(ui::kOptionsScreens.begin(), ui::kOptionsScreens.end(),
					[&p_c](std::string_view p_screen) { return shown(p_c, p_screen); });
	if (p_c.io.alive && p_c.io.game_input && !menu_up && !p_c.menu.in_menu() && !shown(p_c, kSpawnScreenPcPartition)) {
		recovering_ = false;
		next_ = resume_;
		acted_ = false;
		last_act_s_ = p_now_s;
		if (steps_[resume_].resume_escape) {
			tap(p_c, godot::Key::KEY_ESCAPE);
		}
		return;
	}
	if (p_now_s - last_act_s_ < kMenuFramesRecoverKeySeconds) {
		return;
	}
	if (menu_up) {
		tap(p_c, godot::Key::KEY_ESCAPE);
		last_act_s_ = p_now_s;
	} else if (shown(p_c, kSpawnScreenPcPartition) && shown(p_c, kSpawnButtonScreenPartition) &&
			!shown(p_c, kCustomizeScreenPartitions[0])) {
		tap(p_c, godot::Key::KEY_ENTER);
		last_act_s_ = p_now_s;
	}
}

bool MenuFramesCheck::frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, const NativeMenu &p_menu, double p_now_s,
		const MenuFramesIo &p_io) {
	if (done_) {
		return true;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
	}
	const Ctx c{ r_screens, r_flow, p_menu, p_io };
	record(c);
	const size_t resume = resume_step();
	if (!recovering_ && failure_.empty() && next_ < steps_.size() && !p_io.alive && steps_[resume].resume &&
			(next_ > resume || steps_[resume].resume_escape)) {
		recovering_ = true;
		resume_ = resume;
		acted_ = false;
		++deaths_;
	}
	if (recovering_ && failure_.empty()) {
		recover(c, p_now_s);
	} else if (next_ < steps_.size() && failure_.empty()) {
		Step &step = steps_[next_];
		if (!acted_ && step.ready(c)) {
			step.seen_s = p_now_s;
			acted_ = true;
		} else if (!acted_ && step.repeat_before && next_ > 0 && p_now_s - last_act_s_ >= kMenuFramesRepeatSeconds) {
			steps_[next_ - 1].act(*this, c);
			last_act_s_ = p_now_s;
			++repeats_;
		}
		if (acted_ && p_now_s - step.seen_s >= step.hold_s) {
			step.act(*this, c);
			acted_ = false;
			last_act_s_ = p_now_s;
			++next_;
		}
	}
	if (failure_.empty()) {
		if (r_flow.failed()) {
			failure_ = "the UI flow did not load: " + r_flow.error();
		} else if (!r_screens.errors().empty()) {
			failure_ = "screen error: " + r_screens.errors().front();
		} else if (r_flow.session() != nullptr && !r_flow.session()->flow().errors().empty()) {
			failure_ = "flow error: " + r_flow.session()->flow().errors().front();
		} else if (next_ < steps_.size() && p_now_s - start_s_ > kMenuFramesCheckSeconds) {
			failure_ = "timed out waiting for '" + steps_[next_].name + "'";
		}
	}
	if (failure_.empty() && next_ == steps_.size()) {
		judge();
	}
	if (!failure_.empty() || next_ == steps_.size()) {
		done_ = true;
		passed_ = failure_.empty();
	}
	return done_;
}

std::string MenuFramesCheck::report() const {
	using bf3::core::interpolated_percentile;
	std::string out = "[ui] menu frames check:\n";
	for (const Phase ph : { Phase::Play, Phase::First, Phase::Deploy, Phase::Customize, Phase::Menu, Phase::Options }) {
		std::vector<double> wall;
		std::vector<double> session;
		std::vector<double> ui_ms;
		std::vector<double> views;
		std::vector<double> setup;
		std::vector<double> gpu;
		for (const MenuFrameSample &f : frames_) {
			if (steps_[f.step].phase == ph && counted(f)) {
				wall.push_back(f.wall_ms);
				session.push_back(f.session_ms);
				ui_ms.push_back(f.ui_ms);
				views.push_back(f.views_ms);
				setup.push_back(f.setup_ms);
				gpu.push_back(f.render_gpu_ms);
			}
		}
		if (wall.empty()) {
			continue;
		}
		out += bf3::core::fmt("[ui]   %-13s %5zu frames: wall p50 %.2f p99 %.2f max %.2f ms; session p50 %.2f max %.2f "
							  "(UI p50 %.2f max %.2f); views sync p50 %.2f max %.2f; render setup p50 %.2f; root "
							  "viewport GPU p50 %.2f\n",
				phase_name(ph), wall.size(), interpolated_percentile(wall, 0.5), interpolated_percentile(wall, 0.99),
				*std::max_element(wall.begin(), wall.end()), interpolated_percentile(session, 0.5),
				*std::max_element(session.begin(), session.end()), interpolated_percentile(ui_ms, 0.5),
				*std::max_element(ui_ms.begin(), ui_ms.end()), interpolated_percentile(views, 0.5),
				*std::max_element(views.begin(), views.end()), interpolated_percentile(setup, 0.5),
				interpolated_percentile(gpu, 0.5));
	}
	std::vector<size_t> order(frames_.size());
	std::iota(order.begin(), order.end(), size_t(0));
	std::sort(
			order.begin(), order.end(), [this](size_t a, size_t b) { return frames_[a].wall_ms > frames_[b].wall_ms; });
	size_t listed = 0;
	for (const size_t i : order) {
		const MenuFrameSample &f = frames_[i];
		if (steps_[f.step].phase == Phase::Setup || !counted(f)) {
			continue;
		}
		if (listed++ == kMenuFramesReportFrames) {
			break;
		}
		out += bf3::core::fmt("[ui]   slow frame %zu (%s, %s): wall %.2f ms, session %.2f (UI %.2f), views sync "
							  "%.2f, render setup %.2f, root viewport CPU %.2f GPU %.2f, %u screen(s) opened, %u "
							  "glitch(es) triggered, glitch %s\n",
				i, phase_name(steps_[f.step].phase), steps_[f.step].name.c_str(), f.wall_ms, f.session_ms, f.ui_ms,
				f.views_ms, f.setup_ms, f.render_cpu_ms, f.render_gpu_ms, unsigned(f.opened), unsigned(f.glitches),
				f.glitch_drawn ? "drawn" : "off");
	}
	out += "[ui]   " + std::to_string(glitches_seen_) + " glitch triggers seen, " + std::to_string(glitch_frames_) +
			" frames drawn with the glitch\n";
	out += bf3::core::fmt("[ui]   video memory %.1f MB in play, peak %.1f MB\n", video_play_mb_, video_peak_mb_);
	out += local_play_bytes_ ? bf3::core::fmt("[ui]   DXGI local video memory %.1f MiB in play, peak %.1f MiB\n",
									   video_memory_mib(*local_play_bytes_), video_memory_mib(local_peak_bytes_))
							 : "[ui]   DXGI local video memory: no reading\n";
	out += "[ui]   " + std::to_string(repeats_) + " key press(es) repeated after 3 s without the expected screen\n";
	out += "[ui]   " + std::to_string(deaths_) + " death(s) in play or a menu, " + std::to_string(recovery_frames_) +
			" frames until back in play not counted\n";
	for (const std::string &s : shots_) {
		out += "[ui]   shot " + s + "\n";
	}
	out += "[ui]   gated: every frame of the second visits (deploy screen, CUSTOMIZE, Escape menu, Options); " +
			verdict_ + "\n";
	out += passed_ ? "MENU FRAMES: PASS" : "MENU FRAMES: FAIL " + failure_;
	return out;
}

}
