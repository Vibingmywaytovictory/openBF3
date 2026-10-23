#include "hud_tdm_demo.h"

#include "bf3_localisation.h"
#include "godot_util.h"
#include "hud_items.h"
#include "ui_assets.h"

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/text_db.h"

#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

namespace {

constexpr int kStages = 7;
const char *const kStageNames[kStages] = { "pre_round", "spawn_join", "kill_feed", "kill_card", "spawn_dead", "scoreboard", "round_end" };
constexpr uint8_t kLocal = 0;
constexpr uint8_t kPlayers = 24;
constexpr uint8_t kKiller = 5;
const char *const kPreferred[] = { "m16a4", "ak74m", "m249", "pkp", "m40a5", "sv98", "870", "m4a1", "aek971", "g17", "mp443", "rpg7" };

double now_s() {
	return double(Time::get_singleton()->get_ticks_usec()) * 1e-6;
}

void put_name(ui::HudPlayerName &r_name, const std::string &p_text) {
	r_name = {};
	std::memcpy(r_name.data(), p_text.data(), std::min(p_text.size(), r_name.size() - 1));
}

void fill_list(ui::HudPlayerList &r_list, const ui::HudModel &p_model, int p_team) {
	r_list = {};
	std::vector<uint8_t> slots;
	for (uint8_t s = 0; s < p_model.player_count; ++s) {
		if (p_team == 0 || p_model.players[s].team == p_team) {
			slots.push_back(s);
		}
	}
	std::stable_sort(slots.begin(), slots.end(), [&](uint8_t a, uint8_t b) { return p_model.players[a].score > p_model.players[b].score; });
	for (uint8_t s : slots) {
		r_list.index[r_list.count++] = s;
	}
}

}

Bf3TdmHudDemo::Bf3TdmHudDemo() = default;
Bf3TdmHudDemo::~Bf3TdmHudDemo() = default;

void Bf3TdmHudDemo::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	shot_dir_ = user_arg_value("tdm-shots");
	Window *w = get_window();
	w->set_content_scale_mode(Window::CONTENT_SCALE_MODE_DISABLED);
	w->set_size(Vector2i(1280, 720));
	ColorRect *back = memnew(ColorRect);
	back->set_name("Background");
	back->set_color(Color(0.22f, 0.25f, 0.24f, 1.0f));
	back->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	back->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
	add_child(back);
	Bf3HudItems *m2a = memnew(Bf3HudItems);
	m2a->set_name("HudItems");
	add_child(m2a);
	hud_ = std::make_unique<Bf3TdmHud>();
	std::string error;
	if (!hud_->build(this, Bf3Localisation::current(), Bf3UiAssets::current(), &error)) {
		row("build", false, error);
		finish();
		return;
	}
	started_s_ = now_s();
	UtilityFunctions::print(to_godot("[m2b ui demo] built; shots: " + (shot_dir_.empty() ? std::string("none") : shot_dir_)));
}

void Bf3TdmHudDemo::_exit_tree() {
	if (hud_) {
		hud_->shutdown();
	}
}

void Bf3TdmHudDemo::pick_items() {
	Bf3HudItems *m2a = Object::cast_to<Bf3HudItems>(get_node_or_null("HudItems"));
	const ui::ItemIconIndex *index = m2a != nullptr ? m2a->item_index() : nullptr;
	if (index == nullptr) {
		return;
	}
	std::vector<const ui::ItemIcon *> weapons;
	for (const ui::ItemIcon &icon : index->icons()) {
		if (icon.source == ui::ItemIconSource::Weapon && m2a->icon(icon.item_id).is_valid()) {
			weapons.push_back(&icon);
		}
	}
	std::vector<const ui::ItemIcon *> chosen;
	for (const char *p : kPreferred) {
		for (const ui::ItemIcon *icon : weapons) {
			const std::string base = icon->res_name.substr(icon->res_name.find_last_of('/') + 1);
			if (base == std::string(p) + "_small" && std::find(chosen.begin(), chosen.end(), icon) == chosen.end()) {
				chosen.push_back(icon);
				break;
			}
		}
	}
	for (const ui::ItemIcon *icon : weapons) {
		if (chosen.size() >= 20) {
			break;
		}
		if (std::find(chosen.begin(), chosen.end(), icon) == chosen.end()) {
			chosen.push_back(icon);
		}
	}
	for (const ui::ItemIcon *icon : chosen) {
		items_.push_back(icon->item_id);
		item_names_.push_back(icon->name_sid.empty() ? 0u : ui::text_hash(icon->name_sid));
	}
}

ui::HudModel Bf3TdmHudDemo::stage_model(int p_stage) const {
	ui::HudModel m;
	m.serial = serial_;
	m.local_player = kLocal;
	m.local_team = 1;
	m.player_count = kPlayers;
	const auto item = [&](size_t p_i) { return items_.empty() ? 0u : items_[p_i % items_.size()]; };
	for (uint8_t s = 0; s < kPlayers; ++s) {
		ui::HudPlayer &p = m.players[s];
		put_name(p.name, s == kLocal ? std::string("openBF3") : "Bot " + std::to_string(100 + s).substr(1));
		p.team = int8_t(s % 2 == 0 ? 1 : 2);
		p.kit = int8_t(s % 4);
		p.alive = s != kLocal || p_stage == 2 || p_stage == 5;
		p.local = s == kLocal;
		p.bot = s != kLocal;
		p.kills = int16_t((s * 7) % 13);
		p.deaths = int16_t((s * 5) % 9);
		p.score = int32_t(p.kills) * 100 + int32_t((s * 37) % 90);
		p.ping_ms = uint16_t(s == kLocal ? 0 : 20 + (s * 3) % 40);
	}
	ui::HudTeam &t = m.team;
	t.friendly_team = 1;
	t.enemy_team1 = 2;
	t.team1_name = ui::text_hash("ID_M_US");
	t.team2_name = ui::text_hash("ID_M_RU");
	t.friendly_team_name = t.team1_name;
	t.enemy_team1_name = t.team2_name;
	fill_list(t.sorted_players, m, 0);
	fill_list(t.team1_players, m, 1);
	fill_list(t.team2_players, m, 2);
	t.friendly_team_players = t.team1_players;
	t.enemy_team1_players = t.team2_players;
	t.scoreboard_enabled = p_stage == 5;
	ui::HudKillCounter &kc = m.kill_counter;
	const bool in_round = p_stage >= 2;
	kc.team1_kills = in_round ? (p_stage == 6 ? 100 : 23) : 0;
	kc.team2_kills = in_round ? (p_stage == 6 ? 87 : 19) : 0;
	kc.team1_max_kills = 100;
	kc.team2_max_kills = 100;
	kc.player_team = 1;
	kc.player_team_kills = kc.team1_kills;
	kc.enemy_team_kills = kc.team2_kills;

	if (p_stage >= 2 && p_stage <= 5) {
		ui::HudKillFeed &f = m.messages.kill_message;
		const uint8_t killers[6] = { 3, 8, kLocal, 11, 2, kKiller };
		const uint8_t victims[6] = { 6, 1, 7, 4, 9, kLocal };
		for (uint32_t i = 0; i < 6; ++i) {
			const uint32_t serial = i + 1;
			ui::HudKill &k = f.lines[serial % ui::kHudKillFeedSize];
			k.serial = serial;
			k.killer = killers[i];
			k.victim = victims[i];
			k.item_id = item(i);
			k.headshot = i == 1 || i == 5;
			f.newest_serial = serial;
			if (killers[i] == kLocal) {
				m.messages.local_kill_message.lines[serial % ui::kHudKillFeedSize] = k;
				m.messages.local_kill_message.newest_serial = serial;
			}
		}
	}
	if (p_stage == 0) {
		m.pre_round = { true, true, 7.4f, true };
	}
	if (p_stage == 1 || p_stage == 4) {
		ui::HudSpawnScreen &sp = m.spawn_screen;
		sp.visible = true;
		sp.at_join = p_stage == 1;
		sp.selected_kit = p_stage == 1 ? 0 : 1;
		sp.can_deploy = p_stage == 1;
		sp.seconds_to_deploy = p_stage == 1 ? 0.0f : 4.2f;
		sp.revive_possible = p_stage == 4;
		sp.seconds_revive_left = p_stage == 4 ? 6.8f : 0.0f;
		for (size_t k = 0; k < ui::kHudKitCount; ++k) {
			for (size_t i = 0; i < ui::kHudSpawnKitItems; ++i) {
				sp.kit_items[k][i] = i == 4 ? 0u : item(k * 4 + i);
			}
		}
	}
	if (p_stage == 3) {
		ui::HudKillCard &c = m.kill_card;
		c.serial = 1;
		c.visible = true;
		c.killer = kKiller;
		c.killer_kit = m.players[kKiller].kit;
		c.headshot = true;
		c.item_id = item(5);
		c.weapon_name_sid_hash = item_names_.empty() ? 0u : item_names_[5 % item_names_.size()];
		c.distance_m = 37.4f;
		c.killer_health = 64.0f;
		c.seconds_left = 2.5f;
	}
	if (p_stage == 6) {
		ui::HudEndOfRound &e = m.end_of_round;
		e.visible = true;
		e.winner = 1;
		e.local_won = true;
		e.team_kills = { kc.team1_kills, kc.team2_kills };
		e.kill_limit = 100;
		e.top_players = { t.sorted_players.index[0], t.sorted_players.index[1], t.sorted_players.index[2] };
		e.seconds_to_next_round = 18.0f;
		t.friendly_team_is_winner = true;
	}
	return m;
}

void Bf3TdmHudDemo::_process(double p_delta) {
	(void)p_delta;
	if (done_ || !hud_) {
		return;
	}
	if (stage_ < 0) {
		Bf3Localisation *loc = Bf3Localisation::current();
		Bf3UiAssets *assets = Bf3UiAssets::current();
		Bf3HudItems *m2a = Object::cast_to<Bf3HudItems>(get_node_or_null("HudItems"));
		const bool ready = loc != nullptr && (loc->is_loaded() || loc->has_failed()) && assets != nullptr && (assets->is_loaded() || assets->has_failed()) &&
				m2a != nullptr && m2a->is_loaded();
		if (!ready) {
			if (now_s() - started_s_ > kTdmDemoLoadSeconds) {
				row("load", false, "the localisation, UI assets or HUD item icons did not load within " + std::to_string(int(kTdmDemoLoadSeconds)) + " s");
				finish();
			}
			return;
		}
		pick_items();
		row("load", loc->is_loaded() && assets->is_loaded() && !items_.empty(),
				std::to_string(now_s() - started_s_).substr(0, 5) + " s, " + std::to_string(items_.size()) + " weapon items with an icon");
		stage_ = 0;
		frames_ = 0;
	}
	if (frames_ == 0) {
		++serial_;
	}
	hud_->set_model(stage_model(stage_));
	if (++frames_ < kTdmDemoStageFrames) {
		return;
	}
	save_shot(kStageNames[stage_]);
	check_stage(stage_);
	frames_ = 0;
	if (++stage_ >= kStages) {
		finish();
	}
}

void Bf3TdmHudDemo::check_stage(int p_stage) {
	const TdmHudRecords r = hud_->records();
	const std::string name = kStageNames[p_stage];
	switch (p_stage) {
		case 0:
			row(name, r.pre_round_visible && !r.spawn_screen_visible && !r.round_end_visible, "pre_round_visible " + std::to_string(r.pre_round_visible));
			break;
		case 1: {
			bool clicked = false;
			BaseButton *kit = Object::cast_to<BaseButton>(get_node_or_null("Bf3TdmHud/SpawnScreen/Kit2"));
			BaseButton *deploy = Object::cast_to<BaseButton>(get_node_or_null("Bf3TdmHud/SpawnScreen/Deploy"));
			int kit_click = -2;
			bool deploy_click = false;
			if (kit != nullptr && deploy != nullptr) {
				kit->emit_signal("pressed");
				deploy->emit_signal("pressed");
				kit_click = hud_->take_kit_click();
				deploy_click = hud_->take_deploy_click();
				clicked = kit_click == 2 && deploy_click && hud_->take_kit_click() == -1 && !hud_->take_deploy_click();
			}
			row(name, r.spawn_screen_visible && r.deploy_enabled && hud_->wants_mouse() && clicked,
					"visible " + std::to_string(r.spawn_screen_visible) + ", deploy_enabled " + std::to_string(r.deploy_enabled) + ", kit click " +
							std::to_string(kit_click) + ", deploy click " + std::to_string(deploy_click));
			break;
		}
		case 2:
			row(name, !r.spawn_screen_visible && !r.kill_card_visible && r.model_serial == stage_model(2).serial,
					"spawn screen and kill card hidden; the kill feed is the native HUD's (HudMessage_02)");
			break;
		case 3: {
			const ui::HudModel m = stage_model(3);
			const bool named = std::strncmp(r.kill_card_killer.data(), m.players[kKiller].name.data(), r.kill_card_killer.size()) == 0;
			row(name, r.kill_card_visible && r.kill_card_item == m.kill_card.item_id && r.kill_card_item != 0 && r.kill_card_weapon_named && named,
					std::string("killer '") + r.kill_card_killer.data() + "', item " + std::to_string(r.kill_card_item) + ", weapon named " +
							std::to_string(r.kill_card_weapon_named));
			break;
		}
		case 4: {
			BaseButton *deploy = Object::cast_to<BaseButton>(get_node_or_null("Bf3TdmHud/SpawnScreen/Deploy"));
			if (deploy != nullptr) {
				deploy->emit_signal("pressed");
			}
			const bool taken = hud_->take_deploy_click();
			row(name, r.spawn_screen_visible && !r.deploy_enabled && r.selected_kit == 1 && r.spawn_screen_shown == 2 && !taken,
					"deploy_enabled " + std::to_string(r.deploy_enabled) + ", shown " + std::to_string(r.spawn_screen_shown) + ", early click taken " +
							std::to_string(taken));
			break;
		}
		case 5:
			row(name, r.scoreboard_visible && r.scoreboard_rows == kPlayers && r.scoreboard_team_kills[0] == 23 && r.scoreboard_team_kills[1] == 19,
					std::to_string(r.scoreboard_rows) + " rows, " + std::to_string(r.scoreboard_team_kills[0]) + " : " + std::to_string(r.scoreboard_team_kills[1]));
			break;
		case 6:
			row(name, r.round_end_visible && r.round_end_shown == 1 && !r.scoreboard_visible && hud_->wants_mouse(),
					"visible " + std::to_string(r.round_end_visible) + ", shown " + std::to_string(r.round_end_shown));
			break;
		default:
			break;
	}
}

void Bf3TdmHudDemo::save_shot(const std::string &p_label) {
	if (shot_dir_.empty()) {
		return;
	}
	const Ref<ViewportTexture> tex = get_viewport()->get_texture();
	const Ref<Image> img = tex.is_valid() ? tex->get_image() : Ref<Image>();
	if (img.is_null() || img->is_empty()) {
		row("shot." + p_label, false, "no viewport image (headless?)");
		return;
	}
	const std::filesystem::path dir = std::filesystem::path(std::u8string(shot_dir_.begin(), shot_dir_.end()));
	const std::filesystem::path target = dir / ("tdm_" + p_label + ".png");
	Bf3Localisation *loc = Bf3Localisation::current();
	if (loc == nullptr || !loc->reader().write_guard().allows_write(target)) {
		row("shot." + p_label, false, "refused (inside the install)");
		return;
	}
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const std::u8string u8 = target.u8string();
	const std::string path(u8.begin(), u8.end());
	row("shot." + p_label, img->save_png(to_godot(path)) == OK, path + " (" + std::to_string(img->get_width()) + "x" + std::to_string(img->get_height()) + ")");
}

void Bf3TdmHudDemo::row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	++rows_;
	failed_ += p_pass ? 0 : 1;
	UtilityFunctions::print(to_godot("[m2b ui demo] ROW " + p_name + (p_pass ? " PASS " : " FAIL ") + p_detail));
}

void Bf3TdmHudDemo::finish() {
	if (done_) {
		return;
	}
	if (hud_) {
		const TdmHudRecords r = hud_->records();
		row("lookups", r.missing_fonts == 0 && r.missing_icons == 0 && r.missing_strings == 0 && r.draws > 0,
				"missing fonts " + std::to_string(r.missing_fonts) + ", icons " + std::to_string(r.missing_icons) + ", strings " + std::to_string(r.missing_strings) +
						", " + std::to_string(r.draws) + " models set");
	}
	done_ = true;
	UtilityFunctions::print(to_godot("[m2b ui demo] " + std::string(failed_ == 0 ? "PASS " : "FAIL ") + std::to_string(rows_ - failed_) + "/" + std::to_string(rows_) + " rows"));
	get_tree()->quit(failed_ == 0 ? 0 : 1);
}

}
