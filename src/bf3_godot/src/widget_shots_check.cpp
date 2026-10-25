#include "widget_shots_check.h"
#include "bf3_localisation.h"
#include "godot_util.h"
#include "swf_view.h"
#include "ui_assets.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;
using bf3::core::fmt;

namespace {

constexpr const char *kLevel = "Win32/Levels/MP_Subway/MP_Subway";
constexpr const char *kScoreboard32 = "ui/flow/screen/scoreboards/scoreboardtwoteamshud32screen";
constexpr const char *kPopupGeneric = "ui/flow/screen/popups/popupgeneric";
constexpr uint32_t kOptionsTitleHash = 0x2ad6638eu;
constexpr uint32_t kSaveSettingsHash = 0xbed6cb61u;
constexpr int32_t kRowsPerTeam = 32;
constexpr int32_t kLocalRow = 1;
constexpr int32_t kSettleFrames = 4;
constexpr int32_t kPopupFrames = 30;
constexpr double kStep = 1.0 / 60.0;

struct FootageRow {
	const char *name;
	int32_t kills;
	int32_t deaths;
	int32_t score;
};

constexpr std::array<FootageRow, 16> kFriendly{ { { "BOT_Sgt. Foster", 2, 3, 572 }, { "BOT_Cpl. Nguyen", 4, 2, 430 },
		{ "BOT_Cpl. Perry", 3, 3, 405 }, { "BOT_Maj. Black", 3, 1, 378 }, { "BOT_Pvt. Butler", 3, 2, 360 },
		{ "BOT_Pvt. Jackson", 1, 2, 351 }, { "BOT_Col. Cox", 3, 2, 340 }, { "BOT_Pvt. Walker", 2, 2, 331 },
		{ "BOT_Sgt. Morgan", 3, 1, 330 }, { "BOT_Pvt. Lewis", 3, 1, 330 }, { "BOT_Col. Hayes", 2, 2, 308 },
		{ "BOT_Lt. Barnes", 2, 2, 285 }, { "BOT_Sgt. Patel", 2, 1, 271 }, { "BOT_Capt. Shaw", 0, 3, 259 },
		{ "BOT_Maj. Foster", 1, 3, 243 }, { "BOT_Col. Cooper", 2, 2, 235 } } };

constexpr std::array<FootageRow, 16> kEnemy{ { { "BOT_Maj. Belikov", 6, 2, 717 }, { "BOT_Cpl. Makarov", 5, 2, 677 },
		{ "BOT_Sgt. Grigoryev", 6, 2, 650 }, { "BOT_Capt. Zorin", 4, 1, 491 }, { "BOT_Maj. Gromov", 3, 0, 419 },
		{ "BOT_Capt. Nikiforov", 3, 2, 407 }, { "BOT_Maj. Yermakov", 3, 1, 403 }, { "BOT_Col. Karpov", 3, 1, 394 },
		{ "BOT_Col. Dmitriev", 3, 2, 370 }, { "BOT_Pvt. Sergeyev", 2, 1, 355 }, { "BOT_Col. Tarasov", 3, 2, 350 },
		{ "BOT_Capt. Shishkin", 2, 2, 330 }, { "BOT_Lt. Morozov", 3, 2, 320 }, { "BOT_Maj. Tikhonov", 0, 1, 288 },
		{ "BOT_Pvt. Ivanov", 2, 2, 270 }, { "BOT_Col. Avdeyev", 2, 3, 240 } } };

struct Probe {
	const char *what;
	int32_t x;
	int32_t y;
	bool orange;
	bool bright;
};

std::vector<ui::As2Value> team_rows(const std::array<FootageRow, 16> &p_shown, int32_t p_base, int32_t p_local) {
	std::vector<ui::As2Value> rows;
	for (int32_t i = 0; i < kRowsPerTeam; ++i) {
		const bool shown = size_t(i) < p_shown.size();
		ui::As2Value r = ui::As2Value::object();
		r.set("Index", ui::As2Value::of(p_base + i));
		r.set("Label",
				ui::As2Value::of(shown ? std::string(p_shown[size_t(i)].name) : "BOT_" + std::to_string(p_base + i)));
		r.set("Rank", ui::As2Value::of(0));
		r.set("Voip", ui::As2Value::of(0));
		r.set("Kills", ui::As2Value::of(shown ? p_shown[size_t(i)].kills : 0));
		r.set("Deaths", ui::As2Value::of(shown ? p_shown[size_t(i)].deaths : 1));
		r.set("Score", ui::As2Value::of(shown ? p_shown[size_t(i)].score : 200 - i));
		r.set("LocalSquad", ui::As2Value::of(false));
		r.set("LocalPlayer", ui::As2Value::of(i == p_local));
		r.set("MatchReady", ui::As2Value::of(false));
		rows.push_back(std::move(r));
	}
	return rows;
}

bool is_orange(const Color &p_c) {
	return p_c.r > 0.6f && p_c.g > 0.25f && p_c.g < 0.8f && p_c.b < 0.35f;
}

bool is_bright(const Color &p_c) {
	return p_c.r > 0.7f && p_c.g > 0.7f && p_c.b > 0.7f;
}

}

struct Bf3WidgetShotsCheck::State {
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	std::vector<Row> rows;
	std::string shot_dir;
	int32_t phase = 0;
	int32_t frames = 0;
	std::unique_ptr<bf3::core::EbxLibrary> level;
	std::shared_ptr<const ui::TextDatabase> db;
	ui::HudPopup popup;
	Bf3SwfView *view = nullptr;
	std::unique_ptr<ui::HudScreen> screen;

	void add(const std::string &p_name, bool p_pass, const std::string &p_detail) {
		rows.push_back({ p_name, p_pass, p_detail });
	}

	ui::HudClientState client() const { return ui::hud_text_client(db); }
};

Bf3WidgetShotsCheck::Bf3WidgetShotsCheck() : state_(std::make_unique<State>()) {}

Bf3WidgetShotsCheck::~Bf3WidgetShotsCheck() = default;

void Bf3WidgetShotsCheck::_bind_methods() {}

void Bf3WidgetShotsCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	state_->shot_dir = user_arg_value("shot-dir");
	Window *w = get_window();
	w->set_content_scale_mode(Window::CONTENT_SCALE_MODE_DISABLED);
	w->set_size(Vector2i(1280, 720));
	RenderingServer::get_singleton()->set_default_clear_color(Color(0.0f, 0.0f, 0.0f, 1.0f));
	set_process(true);
}

bool Bf3WidgetShotsCheck::open_scoreboard() {
	State &s = *state_;
	bf3::core::Result<ui::HudScreenAsset> asset = ui::read_hud_screen_asset(*s.level, kScoreboard32);
	s.add("scoreboardtwoteamshud32screen asset", bool(asset), asset ? std::string("read") : asset.error().message);
	if (!asset) {
		return false;
	}
	Bf3UiAssets *assets = Bf3UiAssets::current();
	s.view = memnew(Bf3SwfView);
	s.view->set_playing(false);
	add_child(s.view);
	const bf3::core::Status st = s.view->open(asset->movie);
	s.add("open " + asset->movie, bool(st), st ? std::string("opened") : st.error().message);
	if (!st) {
		return false;
	}
	ui::HudStageRects rects;
	rects.visible = s.view->visible_rect();
	rects.original = s.view->scene()->stage();
	bf3::core::Result<std::unique_ptr<ui::HudScreen>> screen =
			ui::HudScreen::open(*s.view->scene(), *assets->movies(), *asset, assets->swf_fonts(), s.client(), rects);
	s.add("scoreboard screen", bool(screen), screen ? std::string("opened") : screen.error().message);
	if (!screen) {
		return false;
	}
	s.screen = std::move(*screen);
	ui::HudModel m;
	m.team.friendly_team_name = ui::text_hash("ID_M_US");
	m.team.enemy_team1_name = ui::text_hash("ID_M_RU");
	m.kill_counter.player_team_kills = 50;
	m.kill_counter.enemy_team_kills = 61;
	s.screen->initialize(m);
	s.screen->set_focus(true);
	const bool friendly = ui::hud_list_set_direct_rows(*s.screen, "List_01", team_rows(kFriendly, 1000, kLocalRow));
	const bool enemy = ui::hud_list_set_direct_rows(*s.screen, "List_02", team_rows(kEnemy, 2000, -1));
	s.add("32 direct rows per team", friendly && enemy,
			fmt("List_01 %s, List_02 %s", friendly ? "set" : "refused", enemy ? "set" : "refused"));
	s.screen->refresh(m);
	s.screen->advance(kStep);
	const bf3::core::Status advanced = s.view->scene()->advance_seconds(kStep);
	s.add("scoreboard scene advance", bool(advanced), advanced ? std::string("advanced") : advanced.error().message);
	const ui::HudScreenStats &stats = s.screen->stats();
	s.add("scoreboard AS2 paths", stats.as2.missing_paths == 0 && stats.as2.failed_ops == 0,
			fmt("%u missing paths, %u failed ops %s", unsigned(stats.as2.missing_paths), unsigned(stats.as2.failed_ops),
					stats.as2.last_failure.c_str()));
	return true;
}

bool Bf3WidgetShotsCheck::open_popup() {
	State &s = *state_;
	bf3::core::Result<ui::HudScreenAsset> asset = ui::read_hud_screen_asset(*s.level, kPopupGeneric);
	s.add("popupgeneric asset", bool(asset), asset ? std::string("read") : asset.error().message);
	if (!asset) {
		return false;
	}
	s.popup.title = std::string(s.db->find(kOptionsTitleHash).value_or(""));
	s.popup.text = std::string(s.db->find(kSaveSettingsHash).value_or(""));
	s.popup.buttons = { { "ID_M_POPUP_YES", -1 }, { "ID_M_POPUP_NO", -1 } };
	s.add("popup text", s.popup.title == "Options" && s.popup.text == "Save settings?",
			s.popup.title + " / " + s.popup.text);
	Bf3UiAssets *assets = Bf3UiAssets::current();
	s.view = memnew(Bf3SwfView);
	s.view->set_playing(false);
	add_child(s.view);
	const bf3::core::Status st = s.view->open(asset->movie);
	s.add("open " + asset->movie, bool(st), st ? std::string("opened") : st.error().message);
	if (!st) {
		return false;
	}
	ui::HudStageRects rects;
	rects.visible = s.view->visible_rect();
	rects.original = s.view->scene()->stage();
	ui::HudClientState client = s.client();
	client.popup = &s.popup;
	bf3::core::Result<std::unique_ptr<ui::HudScreen>> screen = ui::HudScreen::open(
			*s.view->scene(), *assets->movies(), *asset, assets->swf_fonts(), std::move(client), rects);
	s.add("popup screen", bool(screen), screen ? std::string("opened") : screen.error().message);
	if (!screen) {
		return false;
	}
	s.screen = std::move(*screen);
	s.screen->initialize(ui::HudModel{});
	s.screen->set_focus(true);
	return true;
}

void Bf3WidgetShotsCheck::capture(const char *p_file) {
	State &s = *state_;
	const Ref<Image> window = get_viewport()->get_texture()->get_image();
	const Ref<Image> hud = s.view->capture_hud();
	const bool sized = window.is_valid() && hud.is_valid() && window->get_width() == 1280 &&
			window->get_height() == 720 && hud->get_width() == 1280;
	s.add(fmt("%s capture", p_file), sized,
			window.is_valid() ? fmt("window %dx%d", window->get_width(), window->get_height()) : std::string("none"));
	if (!sized) {
		return;
	}
	const std::array<Probe, 5> probes{ { { "scoreboard local row", 400, 196, true, false },
			{ "scoreboard row 0 not orange", 400, 168, false, false }, { "popup title bar", 640, 244, true, false },
			{ "popup YES selected", 585, 497, false, true }, { "popup NO unselected", 660, 505, false, false } } };
	const bool scoreboard = std::string(p_file).find("scoreboard") != std::string::npos;
	for (size_t i = scoreboard ? 0 : 2; i < (scoreboard ? 2 : probes.size()); ++i) {
		const Probe &p = probes[i];
		const Color c = window->get_pixel(p.x, p.y);
		const bool pass = p.orange ? is_orange(c) : (p.bright ? is_bright(c) : (!is_orange(c) && !is_bright(c)));
		s.add(p.what, pass,
				fmt("rgb %d %d %d at %d,%d", int(c.r * 255.0f + 0.5f), int(c.g * 255.0f + 0.5f),
						int(c.b * 255.0f + 0.5f), p.x, p.y));
	}
	if (s.shot_dir.empty()) {
		return;
	}
	Bf3Localisation *loc = Bf3Localisation::current();
	const std::filesystem::path dir = std::filesystem::path(std::u8string(s.shot_dir.begin(), s.shot_dir.end()));
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	for (const auto &[suffix, img] : { std::pair<const char *, Ref<Image>>{ ".png", window },
				 std::pair<const char *, Ref<Image>>{ "_hud.png", hud } }) {
		const std::filesystem::path target = dir / (std::string(p_file) + suffix);
		const std::u8string u8 = target.u8string();
		const bool ok = loc != nullptr && loc->reader().write_guard().allows_write(target) &&
				img->save_png(to_godot(std::string(u8.begin(), u8.end()))) == OK;
		s.add(std::string("shot ") + p_file + suffix, ok, ok ? std::string("written") : std::string("not written"));
	}
}

void Bf3WidgetShotsCheck::finish() {
	State &s = *state_;
	bool all = !s.rows.empty();
	for (const State::Row &r : s.rows) {
		all = all && r.pass;
		UtilityFunctions::print(
				to_godot(fmt("[widget shots] %s %s: %s", r.pass ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str())));
	}
	UtilityFunctions::print(all ? "WIDGET SHOTS CHECK: PASS" : "WIDGET SHOTS CHECK: FAIL");
	set_process(false);
	get_tree()->quit(all ? 0 : 1);
}

void Bf3WidgetShotsCheck::_process(double) {
	State &s = *state_;
	if (s.phase == 0) {
		Bf3UiAssets *assets = Bf3UiAssets::current();
		Bf3Localisation *loc = Bf3Localisation::current();
		if (assets == nullptr || loc == nullptr) {
			s.add("scene", false, "no Bf3UiAssets or Bf3Localisation in the scene");
			finish();
			return;
		}
		if (assets->has_failed() || loc->has_failed()) {
			s.add("assets", false, to_std(assets->has_failed() ? assets->get_error() : loc->get_error()));
			finish();
			return;
		}
		if (!assets->is_loaded() || !loc->is_loaded()) {
			return;
		}
		s.db = loc->database();
		bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> level =
				bf3::core::EbxLibrary::open(loc->install(), { kLevel });
		s.add("MP_Subway resolver", bool(level) && s.db != nullptr,
				level ? std::string("opened") : level.error().message);
		if (!level || s.db == nullptr) {
			finish();
			return;
		}
		s.level = std::move(*level);
		if (!open_scoreboard()) {
			finish();
			return;
		}
		s.phase = 1;
		s.frames = 0;
		return;
	}
	if (s.phase == 1) {
		if (++s.frames < kSettleFrames) {
			return;
		}
		capture("widgets_scoreboard32");
		s.screen.reset();
		remove_child(s.view);
		s.view->queue_free();
		s.view = nullptr;
		if (!open_popup()) {
			finish();
			return;
		}
		s.phase = 2;
		s.frames = 0;
		return;
	}
	if (s.phase == 2) {
		if (s.frames < kPopupFrames) {
			s.screen->refresh(ui::HudModel{});
			s.screen->advance(kStep);
			if (const bf3::core::Status st = s.view->scene()->advance_seconds(kStep); !st) {
				s.add("popup scene advance", false, st.error().message);
			}
		}
		if (++s.frames < kPopupFrames + kSettleFrames) {
			return;
		}
		const ui::HudScreenStats &stats = s.screen->stats();
		s.add("popup AS2 paths", stats.as2.missing_paths == 0 && stats.as2.failed_ops == 0,
				fmt("%u missing paths, %u failed ops %s", unsigned(stats.as2.missing_paths),
						unsigned(stats.as2.failed_ops), stats.as2.last_failure.c_str()));
		capture("widgets_popupgeneric");
		finish();
	}
}

}
