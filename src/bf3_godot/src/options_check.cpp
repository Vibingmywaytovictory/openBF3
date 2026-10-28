#include "options_check.h"

#include "game_settings.h"
#include "godot_util.h"
#include "input_mapper.h"
#include "level_session_menu.h"
#include "native_flow.h"
#include "native_options.h"
#include "native_screens.h"

#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/ui/settings_effects.h"
#include "bf3/core/ui/swf_scene.h"

#include <godot_cpp/classes/file_access.hpp>

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

namespace {

namespace ui = bf3::core::ui;

constexpr std::string_view kSpawnScreenPartition = "ui/flow/screen/spawnscreenpc";
constexpr std::string_view kVolume = ui::kVolumeOption;
constexpr std::string_view kSensitivity = ui::kMouseSensitivityOption;
constexpr std::string_view kFullscreen = ui::kFullscreenOption;
constexpr godot::Key kCandidateKeys[] = { godot::KEY_K, godot::KEY_L, godot::KEY_O, godot::KEY_P, godot::KEY_U,
	godot::KEY_I, godot::KEY_H, godot::KEY_N, godot::KEY_J, godot::KEY_Y };

std::string_view screen(ui::OptionsTab p_tab) {
	return ui::kOptionsScreens[size_t(p_tab)];
}

std::string in_screen(std::string_view p_path) {
	return std::string(ui::kHudScreenInstance) + "." + std::string(p_path);
}

bool near(double p_a, double p_b) {
	return std::abs(p_a - p_b) < 1e-6;
}

}

bool OptionsCheck::shown(const Ctx &p_c, std::string_view p_partition) {
	const UiScreenSlot s = p_c.screens.find_screen(p_partition);
	return s.screen != nullptr && s.shown;
}

std::string OptionsCheck::shown_text(const Ctx &p_c) {
	std::string out;
	for (size_t i = 0; i < p_c.screens.screen_count(); ++i) {
		const UiScreenSlot s = p_c.screens.screen_slot(i);
		if (s.shown) {
			out += (out.empty() ? "" : " ") + ui::ui_data_category_leaf(s.partition) + (s.focused ? "*" : "");
		}
	}
	return out;
}

std::string OptionsCheck::menu_row_state(const Ctx &p_c, size_t p_row) {
	const UiScreenSlot slot = p_c.screens.find_screen(kIngameMenuPartition);
	const ui::SwfSceneNode *holder =
			slot.screen != nullptr ? slot.screen->scene().find_node(in_screen("List_01.ListHolder")) : nullptr;
	if (holder == nullptr) {
		return "no menu";
	}
	std::vector<const ui::SwfSceneNode *> rows;
	for (const ui::SwfNodeId id : holder->children) {
		const ui::SwfSceneNode *n = slot.screen->scene().node(id);
		if (n != nullptr && n->visible && n->kind == ui::SwfNodeKind::Sprite) {
			rows.push_back(n);
		}
	}
	std::sort(rows.begin(), rows.end(),
			[](const ui::SwfSceneNode *a, const ui::SwfSceneNode *b) { return a->matrix.ty < b->matrix.ty; });
	if (p_row >= rows.size() || rows[p_row]->timeline == nullptr) {
		return "no row";
	}
	std::string out;
	for (const ui::SwfFrameLabel &l : rows[p_row]->timeline->labels) {
		if (l.frame <= rows[p_row]->frame) {
			out = l.name;
		}
	}
	return out;
}

void OptionsCheck::open_options(std::vector<Step> &r_steps, const char *p_name) {
	using K = godot::Key;
	r_steps.push_back({ p_name,
			[](OptionsCheck &, const Ctx &c) {
				return shown(c, kIngameMenuPartition) && c.menu.in_menu() && !shown(c, kPopupGenericPartition);
			},
			[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_UP); } });
	r_steps.push_back(
			{ "Up highlights QUIT", [](OptionsCheck &, const Ctx &c) { return menu_row_state(c, 5) == "selected"; },
					[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_UP); } });
	r_steps.push_back(
			{ "Up highlights OPTIONS", [](OptionsCheck &, const Ctx &c) { return menu_row_state(c, 4) == "selected"; },
					[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_ENTER); } });
}

void OptionsCheck::tap(const Ctx &p_c, godot::Key p_key, int32_t p_times) {
	for (int32_t i = 0; i < p_times; ++i) {
		p_c.io.press(p_key, true);
		p_c.io.press(p_key, false);
	}
}

void OptionsCheck::click_clip(const Ctx &p_c, std::string_view p_partition, const std::string &p_path) {
	const std::optional<godot::Vector2> at = p_c.screens.clip_centre(p_partition, p_path);
	if (!at) {
		failure_ = p_path + " is not drawn in " + ui::ui_data_category_leaf(p_partition);
		return;
	}
	p_c.io.move(*at);
	p_c.io.click(*at, true);
	p_c.io.click(*at, false);
}

void OptionsCheck::click_tab(const Ctx &p_c, std::string_view p_partition, size_t p_tab) {
	click_clip(p_c, p_partition, in_screen("TabBar_01.tabBarHolder.button_" + std::to_string(p_tab)));
}

std::optional<std::string> OptionsCheck::list_row(const Ctx &p_c, std::string_view p_partition, size_t p_row) {
	const UiScreenSlot slot = p_c.screens.find_screen(p_partition);
	const ui::SwfSceneNode *holder =
			slot.screen != nullptr ? slot.screen->scene().find_node(in_screen("List_01.ListHolder")) : nullptr;
	if (holder == nullptr) {
		failure_ = "no List_01 in " + ui::ui_data_category_leaf(p_partition);
		return std::nullopt;
	}
	std::vector<const ui::SwfSceneNode *> rows;
	for (const ui::SwfNodeId id : holder->children) {
		const ui::SwfSceneNode *n = slot.screen->scene().node(id);
		if (n != nullptr && n->visible && n->kind == ui::SwfNodeKind::Sprite) {
			rows.push_back(n);
		}
	}
	std::sort(rows.begin(), rows.end(),
			[](const ui::SwfSceneNode *a, const ui::SwfSceneNode *b) { return a->matrix.ty < b->matrix.ty; });
	if (p_row >= rows.size()) {
		failure_ = "row " + std::to_string(p_row) + " of " + ui::ui_data_category_leaf(p_partition) +
				" is not drawn (" + std::to_string(rows.size()) + " rows)";
		return std::nullopt;
	}
	return rows[p_row]->path;
}

double OptionsCheck::value(const Ctx &p_c, std::string_view p_option) {
	return p_c.io.settings != nullptr ? p_c.io.settings->store().value(p_option) : std::nan("");
}

double OptionsCheck::applied(const Ctx &p_c, std::string_view p_option) {
	return p_c.io.settings != nullptr ? p_c.io.settings->store().applied_value(p_option) : std::nan("");
}

void OptionsCheck::click_row(const Ctx &p_c, ui::OptionsTab p_tab, std::string_view p_source) {
	size_t row = 0;
	size_t at = 0;
	bool found = false;
	for (const ui::OptionRowData &d : p_c.options.model().rows) {
		if (d.tab == p_tab && d.list == "List_01") {
			if (d.source == p_source) {
				row = at;
				found = true;
			}
			++at;
		}
	}
	if (!found) {
		failure_ = "no " + std::string(p_source) + " row in List_01 of " + ui::ui_data_category_leaf(screen(p_tab));
		return;
	}
	if (const std::optional<std::string> path = list_row(p_c, screen(p_tab), row)) {
		click_clip(p_c, screen(p_tab), *path + ".hit");
	}
}

void OptionsCheck::check_file_value(const Ctx &p_c, std::string_view p_option, double p_want) {
	ui::SettingsStore probe = p_c.io.settings->store();
	probe.reset_to_defaults();
	const std::string text = to_std(godot::FileAccess::get_file_as_string(to_godot(p_c.io.settings_path)));
	if (bf3::core::Result<uint32_t> r = probe.load_text(text); !r) {
		failure_ = "the saved settings file does not load: " + r.error().message;
		return;
	}
	notes_.push_back("file " + p_c.io.settings_path + " read back: " + std::string(p_option) + " " +
			std::to_string(probe.value(p_option)));
	if (!near(probe.value(p_option), p_want)) {
		failure_ = "the saved file reads back " + std::string(p_option) + " " + std::to_string(probe.value(p_option)) +
				", not " + std::to_string(p_want);
	}
}

void OptionsCheck::pick_key(const Ctx &p_c) {
	const ui::KeyBindingTable &t = p_c.io.settings->store().key_bindings();
	const ui::KeyBindingTab *tab = t.tab_of_map("infantry");
	if (tab == nullptr || t.tabs().empty() || &t.tabs()[0] != tab) {
		failure_ = "the first key bindings tab is not the infantry map";
		return;
	}
	for (size_t i = 0; i < tab->rows.size(); ++i) {
		if (tab->rows[i].concept_id == "ConceptJump" && !tab->rows[i].negative) {
			jump_row_ = int32_t(i);
			old_idk_ = tab->rows[i].inputs[size_t(ui::KeyColumn::Keyboard)];
		}
	}
	const ui::KeyBindingTab *all = t.tab_of_map("default");
	if (all == nullptr) {
		failure_ = "no key bindings tab holds the default map";
		return;
	}
	const size_t all_tab = size_t(all - t.tabs().data());
	for (const godot::Key k : kCandidateKeys) {
		const std::string idk = ui_idk_of_key(k);
		if (!idk.empty() && t.holders(0, ui::KeyColumn::Keyboard, idk).empty() &&
				t.holders(uint8_t(all_tab), ui::KeyColumn::Keyboard, idk).empty()) {
			new_key_ = k;
			new_idk_ = idk;
			break;
		}
	}
	if (jump_row_ < 0 || new_idk_.empty() || old_idk_.empty()) {
		failure_ = "no jump row, jump key or free key in the infantry tab";
	}
}

bool OptionsCheck::file_is_saved(const Ctx &p_c) {
	const godot::String path = to_godot(p_c.io.settings_path);
	if (!godot::FileAccess::file_exists(path)) {
		return false;
	}
	return to_std(godot::FileAccess::get_file_as_string(path)) == p_c.io.settings->store().saved_text();
}

void OptionsCheck::check_file(const Ctx &p_c) {
	const std::string text = to_std(godot::FileAccess::get_file_as_string(to_godot(p_c.io.settings_path)));
	ui::SettingsStore probe = p_c.io.settings->store();
	probe.reset_to_defaults();
	if (bf3::core::Result<uint32_t> r = probe.load_text(text); !r) {
		failure_ = "the saved settings file does not load: " + r.error().message;
		return;
	}
	const ui::KeyBindingTab &tab = probe.key_bindings().tabs()[0];
	const std::string jump = tab.rows[size_t(jump_row_)].inputs[size_t(ui::KeyColumn::Keyboard)];
	notes_.push_back("file " + p_c.io.settings_path + " read back: Volume " + std::to_string(probe.value(kVolume)) +
			", MouseSensitivity " + std::to_string(probe.value(kSensitivity)) + ", jump " + jump);
	if (!near(probe.value(kVolume), saved_volume_) || !near(probe.value(kSensitivity), saved_sensitivity_) ||
			jump != new_idk_) {
		failure_ = "the saved file reads back other values: " + notes_.back();
	}
}

void OptionsCheck::game_tap(const Ctx &p_c, godot::Key p_key) {
	p_c.io.game_key(p_key, true);
	p_c.io.game_key(p_key, false);
}

void OptionsCheck::check_play(const Ctx &p_c) {
	const uint32_t with_new = p_c.io.jump_presses - jumps_before_;
	const ui::SettingsEffects e = ui::settings_effects(p_c.io.settings->store());
	const float look = kBaseLookRadiansPerCount * e.look_scale;
	notes_.push_back("in play through the session's mapper: " + old_idk_ + " then " + new_idk_ + " give " +
			std::to_string(with_new) + " jump, the soldier airborne " + std::to_string(max_air_ticks_) +
			" ticks, look " + std::to_string(p_c.io.look_sensitivity) + " rad/count (" + std::to_string(look) +
			" wanted), master gain " +
			(p_c.io.master_gain ? std::to_string(*p_c.io.master_gain) : std::string("none")) + " (" +
			std::to_string(e.bus_gains[size_t(bf3::core::audio::MasterBus::GameMixMain)]) + " wanted)");
	if (with_new != 1 || max_air_ticks_ == 0) {
		failure_ = "the rebound jump key does not act in play: " + notes_.back();
	} else if (std::abs(p_c.io.look_sensitivity - look) > 1e-7f || !(e.look_scale > 1.0f)) {
		failure_ = "the mouse sensitivity is not applied: " + notes_.back();
	} else if (!p_c.io.master_gain ||
			std::abs(*p_c.io.master_gain - e.bus_gains[size_t(bf3::core::audio::MasterBus::GameMixMain)]) > 1e-6f ||
			!(*p_c.io.master_gain < 1.0f)) {
		failure_ = "the master volume is not applied: " + notes_.back();
	}
}

std::vector<OptionsCheck::Step> OptionsCheck::steps() {
	using K = godot::Key;
	const auto none = [](OptionsCheck &, const Ctx &) {};
	const auto menu_up = [](OptionsCheck &, const Ctx &c) {
		return shown(c, kIngameMenuPartition) && c.menu.in_menu() && !shown(c, kPopupGenericPartition);
	};
	const auto tab_shown = [](ui::OptionsTab p_tab) {
		return [p_tab](OptionsCheck &, const Ctx &c) {
			return shown(c, screen(p_tab)) && c.options.model().selected_tab == uint8_t(p_tab);
		};
	};
	const auto to_tab = [](ui::OptionsTab p_from, size_t p_to) {
		return [p_from, p_to](OptionsCheck &r_self, const Ctx &c) { r_self.click_tab(c, screen(p_from), p_to); };
	};
	const auto popup = [](OptionsCheck &, const Ctx &c) { return shown(c, kPopupGenericPartition); };
	std::vector<Step> s;
	s.push_back({ "spawn screen up", [](OptionsCheck &, const Ctx &c) { return shown(c, kSpawnScreenPartition); },
			[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_ENTER); } });
	s.push_back({ "deployed",
			[](OptionsCheck &, const Ctx &c) {
				return c.io.alive && c.io.game_input && !shown(c, kSpawnScreenPartition);
			},
			[](OptionsCheck &r_self, const Ctx &c) {
				r_self.volume_default_ = value(c, kVolume);
				r_self.sensitivity_default_ = value(c, kSensitivity);
				r_self.pick_key(c);
				tap(c, K::KEY_ESCAPE);
			},
			nullptr, true });
	open_options(s, "Escape opens the menu");
	s.push_back({ "OPTIONS opens CONTROLS", tab_shown(ui::OptionsTab::Controls), to_tab(ui::OptionsTab::Controls, 1),
			"controls" });
	s.push_back(
			{ "GAMEPLAY tab", tab_shown(ui::OptionsTab::Gameplay), to_tab(ui::OptionsTab::Gameplay, 2), "gameplay" });
	s.push_back({ "AUDIO tab", tab_shown(ui::OptionsTab::Audio),
			[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_LEFT, kOptionsCheckSteps); }, "audio" });
	s.push_back({ "Left lowers the master volume",
			[](OptionsCheck &r_self, const Ctx &c) { return value(c, kVolume) < r_self.volume_default_; },
			to_tab(ui::OptionsTab::Audio, 0) });
	s.push_back({ "CONTROLS again", tab_shown(ui::OptionsTab::Controls), [](OptionsCheck &r_self, const Ctx &c) {
					 r_self.click_row(c, ui::OptionsTab::Controls, kSensitivity);
					 tap(c, K::KEY_RIGHT, kOptionsCheckSteps);
				 } });
	s.push_back({ "Right raises the mouse sensitivity",
			[](OptionsCheck &r_self, const Ctx &c) { return value(c, kSensitivity) > r_self.sensitivity_default_; },
			to_tab(ui::OptionsTab::Controls, 3) });
	s.push_back({ "VIDEO tab", tab_shown(ui::OptionsTab::Video), to_tab(ui::OptionsTab::Video, 4), "video" });
	s.push_back({ "KEY BINDINGS tab",
			[](OptionsCheck &, const Ctx &c) {
				return shown(c, screen(ui::OptionsTab::KeyBindings)) &&
						c.options.model().selected_tab == uint8_t(ui::OptionsTab::KeyBindings) &&
						c.options.model().key_tab == 0;
			},
			[](OptionsCheck &r_self, const Ctx &c) {
				if (const std::optional<std::string> path =
								r_self.list_row(c, screen(ui::OptionsTab::KeyBindings), size_t(r_self.jump_row_))) {
					r_self.click_clip(c, screen(ui::OptionsTab::KeyBindings), *path + ".hit_col_0");
				}
			},
			"keybindings" });
	s.push_back({ "the jump row's keyboard cell opens the press-a-key popup",
			[](OptionsCheck &, const Ctx &c) { return shown(c, kPopupBindKeyPartition) && c.options.capturing(); },
			[](OptionsCheck &r_self, const Ctx &c) { tap(c, r_self.new_key_); }, "bind_key" });
	s.push_back({ "the pressed key is bound to jump",
			[](OptionsCheck &r_self, const Ctx &c) {
				const ui::KeyBindingTab &tab = c.io.settings->store().key_bindings().tabs()[0];
				return !shown(c, kPopupBindKeyPartition) &&
						tab.rows[size_t(r_self.jump_row_)].inputs[size_t(ui::KeyColumn::Keyboard)] == r_self.new_idk_;
			},
			[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_ESCAPE); }, "bound" });
	s.push_back({ "leaving asks Save settings?", popup,
			[](OptionsCheck &r_self, const Ctx &c) {
				r_self.saved_volume_ = value(c, kVolume);
				r_self.saved_sensitivity_ = value(c, kSensitivity);
				tap(c, K::KEY_ENTER);
			},
			"save_settings" });
	s.push_back({ "YES saves the scratch settings file and returns to the menu",
			[menu_up](OptionsCheck &r_self, const Ctx &c) {
				return menu_up(r_self, c) && c.options.stats().saves == 1 && r_self.file_is_saved(c);
			},
			[](OptionsCheck &r_self, const Ctx &c) {
				r_self.check_file(c);
				r_self.saved_text_ = to_std(godot::FileAccess::get_file_as_string(to_godot(c.io.settings_path)));
			} });
	open_options(s, "the menu is back");
	s.push_back({ "OPTIONS again", tab_shown(ui::OptionsTab::Controls), to_tab(ui::OptionsTab::Controls, 2) });
	s.push_back({ "AUDIO again", tab_shown(ui::OptionsTab::Audio),
			[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_LEFT, kOptionsCheckSteps); } });
	s.push_back({ "the volume drops again",
			[](OptionsCheck &r_self, const Ctx &c) { return value(c, kVolume) < r_self.saved_volume_; },
			[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_ESCAPE); } });
	s.push_back({ "Save settings? again", popup, [](OptionsCheck &, const Ctx &c) {
					 tap(c, K::KEY_RIGHT);
					 tap(c, K::KEY_ENTER);
				 } });
	s.push_back({ "NO reverts and leaves the file unchanged",
			[menu_up](OptionsCheck &r_self, const Ctx &c) {
				const std::string text = to_std(godot::FileAccess::get_file_as_string(to_godot(c.io.settings_path)));
				return menu_up(r_self, c) && c.options.stats().reverts == 1 &&
						near(value(c, kVolume), r_self.saved_volume_) && text == r_self.saved_text_;
			},
			none });
	open_options(s, "the menu is back again");
	s.push_back({ "OPTIONS a third time", tab_shown(ui::OptionsTab::Controls), [](OptionsCheck &r_self, const Ctx &c) {
					 r_self.click_clip(c, screen(ui::OptionsTab::Controls), in_screen("Button_02"));
				 } });
	s.push_back({ "RESET TO DEFAULT asks", popup, [](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_ENTER); },
			"reset_popup" });
	s.push_back({ "YES resets every setting and binding to the install defaults",
			[](OptionsCheck &r_self, const Ctx &c) {
				const ui::KeyBindingTab &tab = c.io.settings->store().key_bindings().tabs()[0];
				return !shown(c, kPopupGenericPartition) && c.options.stats().resets == 1 &&
						near(value(c, kVolume), r_self.volume_default_) &&
						near(value(c, kSensitivity), r_self.sensitivity_default_) &&
						tab.rows[size_t(r_self.jump_row_)].inputs[size_t(ui::KeyColumn::Keyboard)] == r_self.old_idk_;
			},
			[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_ESCAPE); } });
	s.push_back({ "Save settings? after the reset", popup, [](OptionsCheck &r_self, const Ctx &c) {
					 r_self.click_clip(c, kPopupGenericPartition, in_screen("PopupButtonsManager_01.holder.btn1"));
				 } });
	s.push_back({ "NO keeps the saved settings and file",
			[menu_up](OptionsCheck &r_self, const Ctx &c) {
				const std::string text = to_std(godot::FileAccess::get_file_as_string(to_godot(c.io.settings_path)));
				return menu_up(r_self, c) && c.options.stats().reverts == 2 &&
						near(value(c, kVolume), r_self.saved_volume_) &&
						near(value(c, kSensitivity), r_self.saved_sensitivity_) && text == r_self.saved_text_;
			},
			none });
	const auto fullscreen_on = [](OptionsCheck &r_self, const Ctx &c) {
		r_self.click_row(c, ui::OptionsTab::Video, kFullscreen);
		tap(c, K::KEY_RIGHT);
	};
	const auto apply = [](OptionsCheck &r_self, const Ctx &c) {
		r_self.click_clip(c, screen(ui::OptionsTab::Video), in_screen("Button_02"));
	};
	open_options(s, "the menu is up for APPLY");
	s.push_back({ "OPTIONS for APPLY", tab_shown(ui::OptionsTab::Controls), to_tab(ui::OptionsTab::Controls, 3) });
	s.push_back({ "VIDEO for APPLY", tab_shown(ui::OptionsTab::Video), fullscreen_on });
	s.push_back({ "Right selects fullscreen",
			[](OptionsCheck &, const Ctx &c) {
				return near(value(c, kFullscreen), 1.0) && near(applied(c, kFullscreen), 0.0);
			},
			apply });
	s.push_back({ "APPLY goes fullscreen and asks to keep it",
			[](OptionsCheck &, const Ctx &c) {
				return shown(c, kPopupDelayPartition) && near(applied(c, kFullscreen), 1.0) &&
						c.options.stats().resolution_timers == 1;
			},
			none, "keep_resolution" });
	s.push_back({ "the timer runs out, the window goes back and the file is unchanged",
			[](OptionsCheck &r_self, const Ctx &c) {
				const std::string text = to_std(godot::FileAccess::get_file_as_string(to_godot(c.io.settings_path)));
				return !shown(c, kPopupDelayPartition) && c.options.stats().resolution_timeouts == 1 &&
						c.options.stats().video_reverts == 1 && near(applied(c, kFullscreen), 0.0) &&
						near(value(c, kFullscreen), 0.0) && shown(c, screen(ui::OptionsTab::Video)) &&
						text == r_self.saved_text_;
			},
			fullscreen_on });
	s.push_back({ "Right selects fullscreen again",
			[](OptionsCheck &, const Ctx &c) {
				return near(value(c, kFullscreen), 1.0) && near(applied(c, kFullscreen), 0.0);
			},
			apply });
	s.push_back({ "APPLY asks again",
			[](OptionsCheck &, const Ctx &c) {
				return shown(c, kPopupDelayPartition) && near(applied(c, kFullscreen), 1.0) &&
						c.options.stats().resolution_timers == 2;
			},
			[](OptionsCheck &r_self, const Ctx &c) {
				r_self.click_clip(c, kPopupDelayPartition, in_screen("PopupButtonsManager_01.holder.btn0"));
			} });
	s.push_back({ "YES keeps fullscreen",
			[](OptionsCheck &, const Ctx &c) {
				return !shown(c, kPopupDelayPartition) && c.options.stats().resolutions_kept == 1 &&
						c.options.stats().resolution_timeouts == 1 && c.options.stats().video_reverts == 1 &&
						near(applied(c, kFullscreen), 1.0) && shown(c, screen(ui::OptionsTab::Video));
			},
			[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_ESCAPE); }, "kept_resolution" });
	s.push_back({ "leaving asks Save settings? for fullscreen", popup,
			[](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_ENTER); } });
	s.push_back({ "YES saves fullscreen to the scratch settings file",
			[menu_up](OptionsCheck &r_self, const Ctx &c) {
				return menu_up(r_self, c) && c.options.stats().saves == 2 && r_self.file_is_saved(c);
			},
			[](OptionsCheck &r_self, const Ctx &c) {
				r_self.check_file_value(c, kFullscreen, 1.0);
				tap(c, K::KEY_ESCAPE);
			} });
	s.push_back({ "in play on the ground",
			[](OptionsCheck &, const Ctx &c) {
				return !shown(c, kIngameMenuPartition) && !c.menu.in_menu() && c.io.game_input && c.io.alive &&
						c.io.air_ticks == 0;
			},
			[](OptionsCheck &r_self, const Ctx &c) {
				r_self.jumps_before_ = c.io.jump_presses;
				r_self.max_air_ticks_ = 0;
				r_self.game_tap(c, K::KEY_SPACE);
			} });
	s.push_back({ "the old jump key does not jump", [](OptionsCheck &, const Ctx &) { return true; },
			[](OptionsCheck &r_self, const Ctx &c) {
				if (c.io.jump_presses != r_self.jumps_before_ || r_self.max_air_ticks_ != 0) {
					r_self.failure_ = r_self.old_idk_ + " still jumps through the session's mapper (" +
							std::to_string(c.io.jump_presses - r_self.jumps_before_) + " presses, airborne " +
							std::to_string(r_self.max_air_ticks_) + " ticks)";
					return;
				}
				r_self.game_tap(c, r_self.new_key_);
			} });
	s.push_back({ "the new key jumps the soldier, sensitivity and volume applied",
			[](OptionsCheck &r_self, const Ctx &c) {
				return c.io.jump_presses == r_self.jumps_before_ + 1 && r_self.max_air_ticks_ > 0;
			},
			[](OptionsCheck &r_self, const Ctx &c) {
				r_self.check_play(c);
				tap(c, K::KEY_ESCAPE);
			} });
	s.push_back({ "the menu opens for QUIT", menu_up, [](OptionsCheck &, const Ctx &c) {
					 tap(c, K::KEY_UP);
					 tap(c, K::KEY_ENTER);
				 } });
	s.push_back({ "QUIT asks", popup, [](OptionsCheck &, const Ctx &c) { tap(c, K::KEY_ENTER); } });
	return s;
}

bool OptionsCheck::frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, const NativeMenu &p_menu,
		const NativeOptions &p_options, double p_now_s, const OptionsCheckIo &p_io) {
	if (done_) {
		return true;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
		last_s_ = p_now_s;
	}
	const Ctx c{ r_screens, r_flow, p_menu, p_options, p_io };
	max_air_ticks_ = std::max(max_air_ticks_, p_io.air_ticks);
	if (p_io.settings == nullptr) {
		failure_ = "the session has no GameSettings";
	}
	if (next_ < steps_.size() && failure_.empty()) {
		Step &step = steps_[next_];
		if (!acted_) {
			if (step.ready(*this, c)) {
				step.seen_s = p_now_s;
				acted_ = true;
			} else if (step.repeat_before && next_ > 0 && p_now_s - last_s_ >= kOptionsCheckHoldSeconds) {
				steps_[next_ - 1].act(*this, c);
				last_s_ = p_now_s;
				++step.repeats;
			}
		}
		if (acted_ && failure_.empty() && p_now_s - step.seen_s >= kOptionsCheckHoldSeconds) {
			if (step.shot != nullptr && !shot_dir_.empty() && p_io.shot) {
				const std::string path = shot_dir_ + "/options_" + step.shot + ".png";
				shots_.push_back(path + (p_io.shot(path) ? "" : " FAILED"));
			}
			step.act(*this, c);
			acted_ = false;
			last_s_ = p_now_s;
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
		} else if (next_ < steps_.size() && p_now_s - start_s_ > kOptionsCheckSeconds) {
			const NativeOptionsStats &st = p_options.stats();
			failure_ = std::string("timed out waiting for '") + steps_[next_].name + "' (volume " +
					std::to_string(value(c, kVolume)) + ", sensitivity " + std::to_string(value(c, kSensitivity)) +
					", tab " + std::to_string(p_options.model().selected_tab) + ", values " +
					std::to_string(st.values) + ", saves " + std::to_string(st.saves) + ", reverts " +
					std::to_string(st.reverts) + ", resets " + std::to_string(st.resets) + ", captures " +
					std::to_string(st.captures) + ", bound " + std::to_string(st.keys_bound) + ", shown " +
					shown_text(c) + ", menu rows 4 " + menu_row_state(c, 4) + " 5 " + menu_row_state(c, 5) + ")";
		}
	}
	if (r_flow.session() != nullptr) {
		const std::vector<std::string> &log = r_flow.session()->log();
		flow_seen_ = flow_seen_ > log.size() ? 0 : flow_seen_;
		for (size_t i = flow_seen_; i < log.size(); ++i) {
			const std::string &l = log[i];
			if (l.starts_with("push ") || l.starts_with("close ") || l.starts_with("logic ")) {
				flow_log_.push_back(l);
			}
		}
		flow_seen_ = log.size();
		if (flow_log_.size() > kOptionsCheckFlowLog) {
			flow_log_.erase(flow_log_.begin(), flow_log_.end() - kOptionsCheckFlowLog);
		}
	}
	if (!failure_.empty() || next_ == steps_.size()) {
		done_ = true;
		passed_ = failure_.empty();
	}
	return done_;
}

std::string OptionsCheck::report() const {
	std::string out = "[ui] options check:\n";
	for (const Step &s : steps_) {
		out += "[ui]   " + std::string(s.name) + ": " +
				(s.seen_s >= 0.0 ? "at " + std::to_string(s.seen_s - start_s_) + " s" : std::string("not seen")) +
				(s.repeats != 0 ? ", the step before repeated " + std::to_string(s.repeats) + " times"
								: std::string()) +
				"\n";
	}
	for (const std::string &n : notes_) {
		out += "[ui]   " + n + "\n";
	}
	for (const std::string &s : shots_) {
		out += "[ui]   shot " + s + "\n";
	}
	if (!passed_) {
		for (const std::string &l : flow_log_) {
			out += "[ui]   flow " + l + "\n";
		}
	}
	out += passed_ ? "OPTIONS: PASS" : "OPTIONS: FAIL " + failure_;
	return out;
}

}
