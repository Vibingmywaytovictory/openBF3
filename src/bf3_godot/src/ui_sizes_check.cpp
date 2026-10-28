#include "ui_sizes_check.h"

#include "deploy_screens.h"
#include "level_session_menu.h"
#include "menu_check.h"
#include "native_flow.h"
#include "native_options.h"
#include "native_screens.h"
#include "swf_view.h"

#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/util/format.h"

#include <cmath>

namespace bf3::godot_side {

namespace {

namespace ui = bf3::core::ui;
using bf3::core::fmt;

inline constexpr std::string_view kSpawnScreenPartition = ui::kSpawnScreenPcPartition;
inline constexpr int32_t kSettledFrames = 5;

std::string_view options_screen(ui::OptionsTab p_tab) {
	return ui::kOptionsScreens[size_t(p_tab)];
}

std::string size_text(godot::Vector2i p_size) {
	return std::to_string(p_size.x) + "x" + std::to_string(p_size.y);
}

bool near(double p_a, double p_b) {
	return std::abs(p_a - p_b) <= kUiSizesFitTolerance * std::max(1.0, std::abs(p_b));
}

}

bool UiSizesCheck::shown(const Ctx &p_c, std::string_view p_partition) {
	const UiScreenSlot s = p_c.screens.find_screen(p_partition);
	return s.screen != nullptr && s.shown;
}

bool UiSizesCheck::options_tab_shown(const Ctx &p_c, size_t p_tab) {
	return p_c.options != nullptr && shown(p_c, options_screen(ui::OptionsTab(p_tab))) &&
			p_c.options->model().selected_tab == uint8_t(p_tab);
}

godot::Vector2i UiSizesCheck::wanted_size(const Ctx &p_c, const UiSizesWindow &p_window) {
	return p_window.fullscreen ? p_c.io.screen_size : godot::Vector2i(p_window.width, p_window.height);
}

ui::UiStage UiSizesCheck::rule(const Ctx &p_c) {
	return ui::ui_stage_show_all(float(p_c.io.window_size.x), float(p_c.io.window_size.y));
}

bool UiSizesCheck::window_settled(const Ctx &p_c, const UiSizesWindow &p_window) {
	const godot::Vector2i want = wanted_size(p_c, p_window);
	const UiScreenSlot menu = p_c.screens.find_screen(kIngameMenuPartition);
	const bool view_synced = menu.view == nullptr || !menu.shown || menu.view->target_size() == want;
	const bool at_size = p_c.io.window_size == want && p_c.io.fullscreen == p_window.fullscreen && view_synced;
	if (!at_size || settling_ != want) {
		settling_ = at_size ? want : godot::Vector2i();
		settled_frames_ = 0;
		waiting_ = "window " + size_text(p_c.io.window_size) + (p_c.io.fullscreen ? " fullscreen" : " windowed") +
				", canvas " + size_text(godot::Vector2i(int32_t(p_c.io.canvas_size.x), int32_t(p_c.io.canvas_size.y))) +
				", menu view " + (menu.view != nullptr ? size_text(menu.view->target_size()) : std::string("none")) +
				", wanted " + size_text(want) + (p_window.fullscreen ? " fullscreen" : " windowed");
		if (!at_size && now_s_ - last_tap_s_ >= kUiSizesRetapSeconds) {
			p_c.io.set_window(want, p_window.fullscreen);
			last_tap_s_ = now_s_;
		}
		return false;
	}
	return ++settled_frames_ >= kSettledFrames;
}

void UiSizesCheck::check_fit(const Ctx &p_c, std::string_view p_partition, const UiSizesWindow &p_window) {
	const UiScreenSlot slot = p_c.screens.find_screen(p_partition);
	if (slot.view == nullptr || slot.screen == nullptr) {
		failure_ = std::string(ui::ui_data_category_leaf(p_partition)) + " has no view at " + p_window.tag;
		return;
	}
	const ui::UiStage want = rule(p_c);
	const SwfStageFit fit = slot.view->stage_fit();
	const ui::SwfRect vis = slot.view->visible_rect();
	const ui::SwfRect aligned = slot.screen->stage_rects().visible;
	const double canvas_px =
			p_c.io.canvas_size.y > 0.0f ? double(p_c.io.window_size.y) / double(p_c.io.canvas_size.y) : 0.0;
	const double cursor_px = double(p_c.io.cursor_scale) * canvas_px;
	facts_.push_back(
			fmt("%s %s: window %dx%d%s, canvas %.0fx%.0f; the rule: scale %.4f, offset %.1f, %.1f, Stage.visibleRect "
				"x %.1f..%.1f y %.1f..%.1f; the view: scale %.4f x %.4f, offset %.1f, %.1f, visible x %.1f..%.1f y "
				"%.1f..%.1f; "
				"the widgets aligned to x %.1f..%.1f y %.1f..%.1f; cursor %.4f window px per stage unit",
					p_window.tag, std::string(ui::ui_data_category_leaf(p_partition)).c_str(), p_c.io.window_size.x,
					p_c.io.window_size.y, p_c.io.fullscreen ? " fullscreen" : "", double(p_c.io.canvas_size.x),
					double(p_c.io.canvas_size.y), want.scale, want.offset_x, want.offset_y, double(want.visible.x_min),
					double(want.visible.x_max), double(want.visible.y_min), double(want.visible.y_max), fit.scale_x,
					fit.scale_y, fit.offset_x, fit.offset_y, double(vis.x_min), double(vis.x_max), double(vis.y_min),
					double(vis.y_max), double(aligned.x_min), double(aligned.x_max), double(aligned.y_min),
					double(aligned.y_max), cursor_px));
	const bool view_ok = near(fit.scale_x, want.scale) && near(fit.scale_y, want.scale) &&
			near(fit.offset_x, want.offset_x) && near(fit.offset_y, want.offset_y);
	const bool aligned_ok = near(aligned.x_min, want.visible.x_min) && near(aligned.x_max, want.visible.x_max) &&
			near(aligned.y_min, want.visible.y_min) && near(aligned.y_max, want.visible.y_max);
	const bool cursor_ok =
			!p_c.io.cursor_shown || std::abs(cursor_px - want.scale) <= kUiSizesCursorTolerance * want.scale;
	if (!view_ok) {
		failure_ = "the view does not fit the stage by the rule: " + facts_.back();
	} else if (!aligned_ok) {
		failure_ = "the widgets do not align to the rule's Stage.visibleRect: " + facts_.back();
	} else if (!cursor_ok) {
		failure_ = "the cursor is not drawn at the stage scale: " + facts_.back();
	}
}

void UiSizesCheck::mouse_at(
		const Ctx &p_c, std::string_view p_what, std::optional<ui::SwfPoint> p_stage, bool p_click) {
	if (!p_stage) {
		failure_ = std::string(p_what) + " is not drawn";
		return;
	}
	const ui::SwfPoint px = ui::ui_stage_to_window(rule(p_c), *p_stage);
	const godot::Vector2 at(px.x, px.y);
	facts_.push_back(fmt("%s at window px %.1f, %.1f (stage %.1f, %.1f)", std::string(p_what).c_str(), double(at.x),
			double(at.y), double(p_stage->x), double(p_stage->y)));
	p_c.io.mouse(at, 0, false);
	if (p_click) {
		p_c.io.mouse(at, int32_t(godot::MOUSE_BUTTON_LEFT), true);
		p_c.io.mouse(at, int32_t(godot::MOUSE_BUTTON_LEFT), false);
	}
}

std::vector<UiSizesCheck::Step> UiSizesCheck::steps() const {
	using K = godot::Key;
	const auto tap = [](K p_key) {
		return [p_key](UiSizesCheck &, const Ctx &c) {
			c.io.press(p_key, true);
			c.io.press(p_key, false);
		};
	};
	const auto menu_up = [](UiSizesCheck &, const Ctx &c) {
		return shown(c, kIngameMenuPartition) && c.menu.in_menu() && !shown(c, kPopupGenericPartition);
	};
	const auto menu_row = [](size_t p_row) {
		return [p_row](const Ctx &c) {
			return menu_row_stage_point(c.screens.find_screen(kIngameMenuPartition), p_row);
		};
	};
	const auto deploy_back = [](UiSizesCheck &, const Ctx &c) {
		return !shown(c, kIngameMenuPartition) && shown(c, kSpawnScreenPartition) &&
				shown(c, kSpawnButtonScreenPartition);
	};
	std::vector<Step> s;
	const UiSizesWindow &first = kUiSizesWindows.front();
	s.push_back({ "spawn screen up at 1280x720",
			[first](UiSizesCheck &r_self, const Ctx &c) {
				return shown(c, kSpawnScreenPartition) && r_self.window_settled(c, first);
			},
			[first](UiSizesCheck &r_self, const Ctx &c) {
				r_self.check_fit(c, kSpawnScreenPartition, first);
				c.io.press(K::KEY_ESCAPE, true);
				c.io.press(K::KEY_ESCAPE, false);
			},
			std::string("deploy_") + first.tag });
	const size_t sizes = kUiSizesWindows.size() - 1;
	for (size_t i = 0; i < sizes; ++i) {
		const UiSizesWindow w = kUiSizesWindows[i];
		const std::string tag = w.tag;
		s.push_back(
				{ tag + ": the menu is up, the window goes " + tag, menu_up, [w](UiSizesCheck &r_self, const Ctx &c) {
					 c.io.set_window(wanted_size(c, w), w.fullscreen);
					 r_self.last_tap_s_ = r_self.now_s_;
				 } });
		s.push_back({ tag + ": the window is " + tag + " with the menu open",
				[w, menu_up](UiSizesCheck &r_self, const Ctx &c) {
					return menu_up(r_self, c) && r_self.window_settled(c, w);
				},
				[w, menu_row](UiSizesCheck &r_self, const Ctx &c) {
					r_self.check_fit(c, kIngameMenuPartition, w);
					r_self.mouse_at(c, std::string(w.tag) + " SCOREBOARD", menu_row(kUiSizesScoreboardRow)(c), false);
				},
				"menu_" + tag });
		s.push_back({ tag + ": the mouse over SCOREBOARD turns it over",
				[](UiSizesCheck &, const Ctx &c) {
					return menu_row_label(c.screens.find_screen(kIngameMenuPartition), kUiSizesScoreboardRow) == "over";
				},
				[w, menu_row](UiSizesCheck &r_self, const Ctx &c) {
					r_self.mouse_at(c, std::string(w.tag) + " OPTIONS", menu_row(kUiSizesOptionsRow)(c), true);
				} });
		s.push_back({ tag + ": a click on OPTIONS opens CONTROLS",
				[](UiSizesCheck &, const Ctx &c) { return options_tab_shown(c, size_t(ui::OptionsTab::Controls)); },
				[w](UiSizesCheck &r_self, const Ctx &c) {
					const std::string_view controls = options_screen(ui::OptionsTab::Controls);
					r_self.check_fit(c, controls, w);
					r_self.mouse_at(c, std::string(w.tag) + " the VIDEO tab",
							c.screens.clip_stage_centre(controls,
									"instance1.TabBar_01.tabBarHolder.button_" + std::to_string(kUiSizesVideoTab)),
							true);
				},
				"options_" + tag });
		s.push_back({ tag + ": a click on VIDEO shows it",
				[](UiSizesCheck &, const Ctx &c) { return options_tab_shown(c, size_t(ui::OptionsTab::Video)); },
				tap(K::KEY_ESCAPE), "video_" + tag });
		s.push_back({ tag + ": Escape leaves Options to the menu",
				[menu_up](UiSizesCheck &r_self, const Ctx &c) {
					return menu_up(r_self, c) && !shown(c, options_screen(ui::OptionsTab::Video));
				},
				[w, menu_row](UiSizesCheck &r_self, const Ctx &c) {
					r_self.mouse_at(c, std::string(w.tag) + " RESUME", menu_row(kUiSizesResumeRow)(c), true);
				},
				std::string(),
				[](UiSizesCheck &r_self, const Ctx &c) {
					if (shown(c, kPopupGenericPartition) &&
							r_self.now_s_ - r_self.last_tap_s_ >= kUiSizesRetapSeconds) {
						c.io.press(K::KEY_ENTER, true);
						c.io.press(K::KEY_ENTER, false);
						r_self.last_tap_s_ = r_self.now_s_;
					}
				} });
		const bool last = i + 1 == sizes;
		s.push_back({ tag + ": a click on RESUME brings the deploy screens back", deploy_back,
				[w, last](UiSizesCheck &r_self, const Ctx &c) {
					r_self.check_fit(c, kSpawnScreenPartition, w);
					const K key = last ? K::KEY_ENTER : K::KEY_ESCAPE;
					c.io.press(key, true);
					c.io.press(key, false);
				},
				"deploy_" + tag });
	}
	const UiSizesWindow wide = kUiSizesWindows[sizes - 1];
	const UiSizesWindow restored = kUiSizesWindows.back();
	s.push_back({ std::string(wide.tag) + ": deployed, the HUD up",
			[](UiSizesCheck &, const Ctx &c) {
				return c.io.alive && c.io.game_input && c.io.hud_visible && !shown(c, kSpawnScreenPartition);
			},
			[wide](UiSizesCheck &r_self, const Ctx &c) {
				r_self.check_fit(c, ui::kHudMpScreenPartition, wide);
				c.io.press(K::KEY_ESCAPE, true);
				c.io.press(K::KEY_ESCAPE, false);
			},
			std::string("hud_") + wide.tag });
	s.push_back({ std::string(wide.tag) + ": the menu over the round, the window goes back to 1280x720", menu_up,
			[restored](UiSizesCheck &r_self, const Ctx &c) {
				c.io.set_window(wanted_size(c, restored), restored.fullscreen);
				r_self.last_tap_s_ = r_self.now_s_;
			} });
	s.push_back({ "1280x720 again with the menu open",
			[restored, menu_up](UiSizesCheck &r_self, const Ctx &c) {
				return menu_up(r_self, c) && r_self.window_settled(c, restored);
			},
			[restored, menu_row](UiSizesCheck &r_self, const Ctx &c) {
				r_self.check_fit(c, kIngameMenuPartition, restored);
				r_self.mouse_at(c, std::string(restored.tag) + " RESUME", menu_row(kUiSizesResumeRow)(c), true);
			},
			std::string("menu_") + restored.tag });
	s.push_back({ "a click on RESUME returns to play at 1280x720",
			[](UiSizesCheck &, const Ctx &c) {
				return !shown(c, kIngameMenuPartition) && c.io.alive && c.io.game_input && !c.io.cursor_shown;
			},
			[](UiSizesCheck &, const Ctx &) {} });
	return s;
}

bool UiSizesCheck::frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, const NativeMenu &p_menu,
		const NativeOptions *p_options, double p_now_s, const UiSizesCheckIo &p_io) {
	if (done_) {
		return true;
	}
	now_s_ = p_now_s;
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
	}
	const Ctx c{ r_screens, r_flow, p_menu, p_options, p_io };
	if (next_ < steps_.size() && failure_.empty()) {
		Step &step = steps_[next_];
		if (!acted_) {
			if (step.ready(*this, c)) {
				step.seen_s = p_now_s;
				acted_ = true;
			} else if (step.waiting) {
				step.waiting(*this, c);
			}
		}
		if (acted_ && failure_.empty() && p_now_s - step.seen_s >= kUiSizesHoldSeconds) {
			if (!step.shot.empty() && !shot_dir_.empty() && p_io.shot) {
				const std::string path = shot_dir_ + "/" + step.shot + ".png";
				shots_.push_back(path + (p_io.shot(path) ? "" : " FAILED") + " at " + size_text(p_io.window_size));
			}
			step.act(*this, c);
			acted_ = false;
			++next_;
			waiting_.clear();
		}
	}
	if (failure_.empty()) {
		if (r_flow.failed()) {
			failure_ = "the UI flow did not load: " + r_flow.error();
		} else if (!r_screens.errors().empty()) {
			failure_ = "screen error: " + r_screens.errors().front();
		} else if (next_ < steps_.size() && p_now_s - start_s_ > kUiSizesCheckSeconds) {
			failure_ = "timed out waiting for '" + steps_[next_].name + "'" +
					(waiting_.empty() ? std::string() : ", last seen: " + waiting_);
		}
	}
	if (!failure_.empty() || next_ == steps_.size()) {
		done_ = true;
		passed_ = failure_.empty();
	}
	return done_;
}

std::string UiSizesCheck::report() const {
	std::string out = "[ui] window sizes check:\n";
	for (const Step &s : steps_) {
		out += "[ui]   " + s.name + ": " +
				(s.seen_s >= 0.0 ? fmt("at %.2f s", s.seen_s - start_s_) : std::string("not seen")) + "\n";
	}
	for (const std::string &f : facts_) {
		out += "[ui]   fact " + f + "\n";
	}
	for (const std::string &s : shots_) {
		out += "[ui]   shot " + s + "\n";
	}
	out += passed_ ? "UI SIZES: PASS" : "UI SIZES: FAIL " + failure_;
	return out;
}

}
