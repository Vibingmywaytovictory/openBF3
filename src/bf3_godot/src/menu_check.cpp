#include "menu_check.h"

#include "deploy_screens.h"
#include "level_session_menu.h"
#include "native_flow.h"
#include "native_hud.h"
#include "native_screens.h"
#include "overview_map.h"
#include "swf_view.h"

#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_hit.h"
#include "bf3/core/ui/swf_scene.h"

#include <algorithm>
#include <utility>

namespace bf3::godot_side {

namespace {

namespace ui = bf3::core::ui;

inline constexpr std::string_view kSpawnScreenPartition = "ui/flow/screen/spawnscreenpc";
inline constexpr std::string_view kKillScreenPartition = ui::kKillScreenPartition;

const ui::SwfScene *menu_scene(const UiScreenSlot &p_slot) {
	return p_slot.screen != nullptr ? &p_slot.screen->scene() : nullptr;
}

std::vector<const ui::SwfSceneNode *> menu_rows(const ui::SwfScene &p_scene) {
	std::vector<const ui::SwfSceneNode *> out;
	const ui::SwfSceneNode *holder = p_scene.find_node(std::string(kMenuCheckList) + ".ListHolder");
	if (holder == nullptr) {
		return out;
	}
	for (const ui::SwfNodeId id : holder->children) {
		const ui::SwfSceneNode *c = p_scene.node(id);
		if (c != nullptr && c->visible && c->kind == ui::SwfNodeKind::Sprite) {
			out.push_back(c);
		}
	}
	std::sort(out.begin(), out.end(),
			[](const ui::SwfSceneNode *a, const ui::SwfSceneNode *b) { return a->matrix.ty < b->matrix.ty; });
	return out;
}

std::string frame_label(const ui::SwfSceneNode &p_node) {
	std::string out;
	if (p_node.timeline == nullptr) {
		return out;
	}
	for (const ui::SwfFrameLabel &l : p_node.timeline->labels) {
		if (l.frame <= p_node.frame) {
			out = l.name;
		}
	}
	return out;
}

}

std::string MenuCheck::state_text(const Ctx &p_c, bool p_menu) {
	return std::string("menu ") + (p_menu ? "up" : "down") + ", game input " + (p_c.io.game_input ? "on" : "off") +
			", cursor " + (p_c.io.cursor_shown ? "shown" : "hidden") + ", HUD " +
			(p_c.io.hud_visible ? "shown" : "hidden") + " (" + hud_screens_text(p_c) + "), soldier " +
			(p_c.io.alive ? "alive" : "dead") + ", " + std::to_string(p_c.menu.stats().glitches) + " glitches";
}

std::string MenuCheck::hud_screens_text(const Ctx &p_c) {
	std::string out;
	for (const std::string_view p : kNativeHudPartitions) {
		out += (out.empty() ? "" : " ") + ui::ui_data_category_leaf(p) + (shown(p_c, p) ? "+" : "-");
	}
	return out;
}

bool MenuCheck::shown(const Ctx &p_c, std::string_view p_partition) {
	const UiScreenSlot s = p_c.screens.find_screen(p_partition);
	return s.screen != nullptr && s.shown;
}

std::string menu_row_label(const UiScreenSlot &p_slot, size_t p_row) {
	const ui::SwfScene *scene = menu_scene(p_slot);
	if (scene == nullptr) {
		return "no menu";
	}
	const std::vector<const ui::SwfSceneNode *> rows = menu_rows(*scene);
	return p_row < rows.size() ? frame_label(*rows[p_row]) : std::string("no row");
}

std::optional<ui::SwfPoint> menu_row_stage_point(const UiScreenSlot &p_slot, size_t p_row, bool p_hit) {
	const ui::SwfScene *scene = menu_scene(p_slot);
	if (scene == nullptr || p_slot.view == nullptr) {
		return std::nullopt;
	}
	const std::vector<const ui::SwfSceneNode *> rows = menu_rows(*scene);
	if (p_row >= rows.size()) {
		return std::nullopt;
	}
	const ui::SwfMatrix w = ui::swf_world_matrix(*scene, rows[p_row]->id);
	const float sx = w.a * kMenuCheckRowHitX + w.c * kMenuCheckRowHitY + w.tx;
	const float sy = w.b * kMenuCheckRowHitX + w.d * kMenuCheckRowHitY + w.ty;
	if (p_hit && p_slot.screen->hit_target(sx, sy) != rows[p_row]->path) {
		return std::nullopt;
	}
	return ui::SwfPoint{ sx, sy };
}

std::string MenuCheck::row_state(const Ctx &p_c, size_t p_row) {
	return menu_row_label(p_c.screens.find_screen(kIngameMenuPartition), p_row);
}

std::optional<godot::Vector2> MenuCheck::row_point(const Ctx &p_c, size_t p_row, bool p_hit) {
	const UiScreenSlot slot = p_c.screens.find_screen(kIngameMenuPartition);
	const std::optional<ui::SwfPoint> at = menu_row_stage_point(slot, p_row, p_hit);
	if (!at) {
		return std::nullopt;
	}
	return slot.view->stage_to_canvas(*at);
}

std::optional<godot::Rect2> MenuCheck::overview_rect(const Ctx &p_c) {
	const UiScreenSlot slot = p_c.screens.find_screen(kIngameMenuPartition);
	const ui::SwfScene *scene = menu_scene(slot);
	if (scene == nullptr || slot.view == nullptr) {
		return std::nullopt;
	}
	const std::vector<SwfEngineTarget> targets = slot.view->engine_targets(kOverviewMapRenderer);
	if (targets.empty()) {
		return std::nullopt;
	}
	const ui::SwfRect &b = targets.front().local_bounds;
	const ui::SwfMatrix w = ui::swf_world_matrix(*scene, targets.front().node);
	godot::Rect2 out;
	bool first = true;
	for (const auto &[x, y] : { std::pair{ b.x_min, b.y_min }, std::pair{ b.x_max, b.y_min },
				 std::pair{ b.x_min, b.y_max }, std::pair{ b.x_max, b.y_max } }) {
		const float sx = w.a * x + w.c * y + w.tx;
		const float sy = w.b * x + w.d * y + w.ty;
		const godot::Vector2 at = slot.view->stage_to_canvas(ui::SwfPoint{ sx, sy });
		out = first ? godot::Rect2(at, godot::Vector2()) : out.expand(at);
		first = false;
	}
	return out;
}

void MenuCheck::check_overview_drawn(const Ctx &p_c) {
	const std::optional<godot::Rect2> rect = overview_rect(p_c);
	const godot::Ref<godot::Image> img = p_c.io.capture ? p_c.io.capture() : godot::Ref<godot::Image>();
	if (!rect || img.is_null()) {
		failure_ = "the overview map box or the viewport image is missing";
		return;
	}
	const int32_t x0 = std::max(0, int32_t(rect->position.x));
	const int32_t y0 = std::max(0, int32_t(rect->position.y));
	const int32_t x1 = std::min(img->get_width(), int32_t(rect->get_end().x));
	const int32_t y1 = std::min(img->get_height(), int32_t(rect->get_end().y));
	uint32_t samples = 0;
	uint32_t bright = 0;
	for (int32_t y = y0; y < y1; y += kMenuCheckSampleStep) {
		for (int32_t x = x0; x < x1; x += kMenuCheckSampleStep) {
			const godot::Color c = img->get_pixel(x, y);
			++samples;
			bright += (c.r + c.g + c.b) / 3.0f > kMenuCheckBrightLevel ? 1u : 0u;
		}
	}
	const float fraction = samples != 0 ? float(bright) / float(samples) : 0.0f;
	overview_drawn_ = std::to_string(x1 - x0) + "x" + std::to_string(y1 - y0) + " px box, " +
			std::to_string(int32_t(fraction * 100.0f)) + " % bright";
	overview_drawn_ += ", " + std::to_string(p_c.io.overview_icons) + " icons";
	if (fraction < kMenuCheckOverviewBright) {
		failure_ = "the overview map is not drawn when the menu opens: " + overview_drawn_;
	} else if (p_c.io.alive && p_c.io.overview_icons == 0) {
		failure_ = "the overview map draws no own-soldier marker: " + overview_drawn_;
	}
}

void MenuCheck::tap(const Ctx &p_c, godot::Key p_key) {
	p_c.io.press(p_key, true);
	p_c.io.press(p_key, false);
}

void MenuCheck::click_row(const Ctx &p_c, size_t p_row, bool p_hit) {
	const std::optional<godot::Vector2> at = row_point(p_c, p_row, p_hit);
	if (!at) {
		failure_ = "menu row " + std::to_string(p_row) + " is not under its own hit point";
		return;
	}
	p_c.io.move(*at);
	p_c.io.click(*at, true);
	p_c.io.click(*at, false);
}

std::vector<MenuCheck::Step> MenuCheck::steps() {
	using K = godot::Key;
	const auto press = [](K p_key) { return [p_key](MenuCheck &, const Ctx &c) { tap(c, p_key); }; };
	const auto none = [](MenuCheck &, const Ctx &) {};
	const auto menu_open = [](MenuCheck &r_self, const Ctx &c) {
		const bool menu = shown(c, kIngameMenuPartition) && c.menu.in_menu();
		const bool hud_kept = c.io.hud_visible &&
				std::all_of(kNativeHudPartitions.begin(), kNativeHudPartitions.end(),
						[&c](std::string_view p_partition) { return shown(c, p_partition); });
		const bool open = menu && !c.io.game_input && c.io.cursor_shown && hud_kept &&
				c.menu.stats().glitches > r_self.glitches_at_open_;
		r_self.waiting_ = state_text(c, menu);
		if (open) {
			r_self.glitches_at_open_ = c.menu.stats().glitches;
		}
		return open;
	};
	const auto in_play = [](MenuCheck &r_self, const Ctx &c) {
		const bool menu = shown(c, kIngameMenuPartition) || c.menu.in_menu();
		r_self.waiting_ = state_text(c, menu);
		return !menu && c.io.alive && c.io.game_input && !c.io.cursor_shown && c.io.hud_visible;
	};
	const auto row_is = [](size_t p_row, const char *p_state) {
		return [p_row, p_state](MenuCheck &, const Ctx &c) { return row_state(c, p_row) == p_state; };
	};
	std::vector<Step> s;
	s.push_back({ "spawn screen up", [](MenuCheck &, const Ctx &c) { return shown(c, kSpawnScreenPartition); },
			press(K::KEY_ESCAPE) });
	s.push_back({ "Escape on the deploy screen opens the menu",
			[](MenuCheck &, const Ctx &c) { return shown(c, kIngameMenuPartition) && c.menu.in_menu(); },
			press(K::KEY_ENTER), "deploy_menu" });
	s.push_back({ "RESUME brings the deploy screens back",
			[](MenuCheck &r_self, const Ctx &c) {
				const bool menu = shown(c, kIngameMenuPartition);
				r_self.waiting_ = state_text(c, menu) + ", spawnscreenpc " +
						(shown(c, kSpawnScreenPartition) ? "shown" : "hidden") + ", spawnbuttonscreen " +
						(shown(c, kSpawnButtonScreenPartition) ? "shown" : "hidden");
				return !menu && c.menu.in_menu() && shown(c, kSpawnScreenPartition) &&
						shown(c, kSpawnButtonScreenPartition);
			},
			press(K::KEY_ENTER), "deploy_resumed" });
	s.push_back({ "deployed", [](MenuCheck &, const Ctx &c) { return c.io.alive && !shown(c, kSpawnScreenPartition); },
			press(K::KEY_ESCAPE), nullptr, true });
	s.push_back({ "Escape opens the menu: game input off, cursor on, HUD kept, glitch, overview map drawn", menu_open,
			[](MenuCheck &r_self, const Ctx &c) {
				r_self.check_overview_drawn(c);
				tap(c, K::KEY_DOWN);
			},
			"open" });
	s.push_back({ "Down highlights SCOREBOARD", row_is(1, "selected"), press(K::KEY_DOWN) });
	s.push_back({ "Down skips SQUAD & TEAM to SUICIDE",
			[](MenuCheck &, const Ctx &c) { return row_state(c, 2) == "inactive" && row_state(c, 3) == "selected"; },
			press(K::KEY_DOWN) });
	s.push_back({ "Down highlights OPTIONS", row_is(4, "selected"), press(K::KEY_DOWN) });
	s.push_back({ "Down highlights QUIT",
			[](MenuCheck &, const Ctx &c) { return shown(c, kIngameMenuPartition) && row_state(c, 5) == "selected"; },
			[](MenuCheck &r_self, const Ctx &c) {
				if (const std::optional<godot::Vector2> at = row_point(c, 1)) {
					c.io.move(*at);
				} else {
					r_self.failure_ = "SCOREBOARD is not under its own hit point";
				}
			} });
	s.push_back({ "hover turns SCOREBOARD over", row_is(1, "over"),
			[](MenuCheck &r_self, const Ctx &c) { r_self.click_row(c, 0); }, "hover" });
	s.push_back({ "a click on RESUME returns to play with a glitch",
			[in_play](MenuCheck &r_self, const Ctx &c) {
				return in_play(r_self, c) && c.menu.stats().glitches > r_self.glitches_at_open_;
			},
			press(K::KEY_ESCAPE) });
	s.push_back({ "Escape opens the menu again", menu_open, press(K::KEY_ESCAPE) });
	s.push_back({ "Escape returns to play", in_play, press(K::KEY_ESCAPE) });
	s.push_back({ "the menu opens a third time", menu_open, press(K::KEY_DOWN) });
	s.push_back({ "SCOREBOARD highlighted", row_is(1, "selected"), press(K::KEY_ENTER) });
	s.push_back({ "SCOREBOARD opens the two-team scoreboard and its BACK screen",
			[](MenuCheck &, const Ctx &c) {
				return shown(c, kMenuScoreboardPartition) && shown(c, kMenuScoreboardBackPartition) &&
						!shown(c, kIngameMenuPartition);
			},
			press(K::KEY_ESCAPE), "scoreboard" });
	s.push_back({ "Back pops both back to the menu",
			[](MenuCheck &, const Ctx &c) {
				return shown(c, kIngameMenuPartition) && !shown(c, kMenuScoreboardPartition) &&
						!shown(c, kMenuScoreboardBackPartition);
			},
			press(K::KEY_DOWN) });
	s.push_back({ "SCOREBOARD highlighted again", row_is(1, "selected"), press(K::KEY_DOWN) });
	s.push_back({ "SUICIDE highlighted", row_is(3, "selected"), press(K::KEY_ENTER) });
	s.push_back({ "SUICIDE asks in popupgeneric",
			[](MenuCheck &, const Ctx &c) { return shown(c, kPopupGenericPartition); }, press(K::KEY_ENTER),
			"suicide_popup" });
	s.push_back({ "OK kills the soldier",
			[](MenuCheck &, const Ctx &c) {
				return c.menu.stats().suicides == 1 && !c.io.alive && !shown(c, kPopupGenericPartition);
			},
			none });
	s.push_back({ "the death flow opens the killscreen",
			[](MenuCheck &, const Ctx &c) { return shown(c, kKillScreenPartition); }, none });
	s.push_back({ "the spawn screen follows", [](MenuCheck &, const Ctx &c) { return shown(c, kSpawnScreenPartition); },
			press(K::KEY_ENTER) });
	s.push_back({ "deployed again",
			[](MenuCheck &, const Ctx &c) { return c.io.alive && c.io.game_input && !shown(c, kSpawnScreenPartition); },
			press(K::KEY_ESCAPE), nullptr, true });
	s.push_back({ "the menu opens a fourth time", menu_open, press(K::KEY_UP) });
	s.push_back({ "Up loops to QUIT", row_is(5, "selected"), press(K::KEY_ENTER) });
	s.push_back(
			{ "QUIT asks in popupgeneric", [](MenuCheck &, const Ctx &c) { return shown(c, kPopupGenericPartition); },
					press(K::KEY_ENTER), "quit_popup" });
	s.push_back({ "OK quits", [](MenuCheck &, const Ctx &c) { return c.menu.quit_requested(); }, none });
	return s;
}

bool MenuCheck::frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, const NativeMenu &p_menu, double p_now_s,
		const MenuCheckIo &p_io) {
	if (done_) {
		return true;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
		last_s_ = p_now_s;
	}
	const Ctx c{ r_screens, r_flow, p_menu, p_io };
	if (shown(c, kIngameMenuPartition)) {
		overview_ = p_io.overview;
	}
	if (next_ < steps_.size() && failure_.empty()) {
		Step &step = steps_[next_];
		if (!acted_) {
			if (step.ready(*this, c)) {
				step.seen_s = p_now_s;
				acted_ = true;
			} else if (step.repeat_before && next_ > 0 && p_now_s - last_s_ >= kMenuCheckHoldSeconds) {
				steps_[next_ - 1].act(*this, c);
				last_s_ = p_now_s;
				++step.repeats;
			}
		}
		if (acted_ && failure_.empty() && p_now_s - step.seen_s >= kMenuCheckHoldSeconds) {
			if (step.shot != nullptr && !shot_dir_.empty() && p_io.shot) {
				const std::string path = shot_dir_ + "/menu_" + step.shot + ".png";
				shots_.push_back(path + (p_io.shot(path) ? "" : " FAILED") + ": " +
						state_text(c, shown(c, kIngameMenuPartition)));
			}
			step.act(*this, c);
			acted_ = false;
			last_s_ = p_now_s;
			++next_;
			waiting_.clear();
		}
	}
	if (failure_.empty()) {
		if (r_flow.failed()) {
			failure_ = "the UI flow did not load: " + r_flow.error();
		} else if (!r_screens.errors().empty()) {
			failure_ = "screen error: " + r_screens.errors().front();
		} else if (r_flow.session() != nullptr && !r_flow.session()->flow().errors().empty()) {
			failure_ = "flow error: " + r_flow.session()->flow().errors().front();
		} else if (next_ < steps_.size() && p_now_s - start_s_ > kMenuCheckSeconds) {
			failure_ = std::string("timed out waiting for '") + steps_[next_].name + "'" +
					(waiting_.empty() ? std::string() : ", last seen: " + waiting_);
		}
	}
	if (!failure_.empty() || next_ == steps_.size()) {
		done_ = true;
		passed_ = failure_.empty();
	}
	return done_;
}

std::string MenuCheck::report() const {
	std::string out = "[ui] menu check:\n";
	for (const Step &s : steps_) {
		out += "[ui]   " + std::string(s.name) + ": " +
				(s.seen_s >= 0.0 ? "at " + std::to_string(s.seen_s - start_s_) + " s" : std::string("not seen")) +
				(s.repeats != 0 ? ", the step before repeated " + std::to_string(s.repeats) + " times"
								: std::string()) +
				"\n";
	}
	out += "[ui]   overview map in the menu: " + overview_ + "; at open " + overview_drawn_ + "\n";
	for (const std::string &s : shots_) {
		out += "[ui]   shot " + s + "\n";
	}
	out += passed_ ? "MENU: PASS" : "MENU: FAIL " + failure_;
	return out;
}

}
