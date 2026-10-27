#include <doctest/doctest.h>

#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/ui/ui_direct_access.h"
#include "bf3/core/ui/ui_input.h"
#include "list.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using ui_retail::Retail;
using ui_retail::retail_or_skip;

namespace {

constexpr std::string_view kScoreboard32 = "ui/flow/screen/scoreboards/scoreboardtwoteamshud32screen";
constexpr std::string_view kScoreboardTwoTeams = "ui/flow/screen/scoreboards/scoreboardtwoteamsscreen";
constexpr std::string_view kSpawnScreenPc = "ui/flow/screen/spawnscreenpc";
constexpr std::string_view kPopupGeneric = "ui/flow/screen/popups/popupgeneric";
constexpr uint32_t kOptionsTitleHash = 0x2ad6638eu;
constexpr uint32_t kSaveSettingsHash = 0xbed6cb61u;

void check_scoreboard_list(const HudScreenAsset &p_asset, std::string_view p_instance, int32_t p_key) {
	const HudWidgetNode *node = p_asset.find(p_instance);
	REQUIRE(node != nullptr);
	REQUIRE(node->list.has_value());
	const HudListBinding &l = *node->list;
	CHECK(l.row_type == "ScoreboardRow");
	CHECK(l.empty_row_type == "emptyRow_28px");
	CHECK(l.navigation_type == "none");
	CHECK(l.unfocus_mode == "DeSelect");
	CHECK(l.disabled_rows.empty());
	CHECK(l.list_index == 0);
	CHECK(l.row_spacing == 0);
	CHECK(l.default_highlighted_row == -1);
	CHECK(l.use_scroll_bar);
	CHECK(l.high_light_on_update);
	CHECK(l.visible);
	CHECK_FALSE(l.clear_list_at_navigation_event);
	CHECK_FALSE(l.data_includes_button_layout);
	CHECK_FALSE(l.fire_highlight_on_mouse_out);
	CHECK_FALSE(l.screen_rotation_enabled);
	CHECK_FALSE(l.send_index_with_event);
	CHECK_FALSE(l.spinner_enabled);
	CHECK_FALSE(l.use_3d_selection);
	CHECK(l.list_query.key == p_key);
	CHECK(l.list_query.category == "ui/uicomponents/uiteamcomp");
	CHECK(l.list_query.use_direct_access);
	CHECK(l.list_query.on_initialize);
}

struct ScreenRig {
	ui_retail::RetailScreen s;
	std::vector<HudWidgetEvent> events;

	As2Clip clip(std::string_view p_path) const {
		return s.hud->clip(std::string(kHudScreenInstance) + "." + std::string(p_path));
	}
	std::string row(int32_t p_index) const {
		return s.row_at(std::string(kHudScreenInstance) + ".List_01.ListHolder", 28.0 * p_index);
	}
	size_t events_of(std::string_view p_instance) const {
		return size_t(std::count_if(
				events.begin(), events.end(), [&](const HudWidgetEvent &p_e) { return p_e.instance == p_instance; }));
	}
	std::string frame_label(const std::string &p_path) const {
		const SwfSceneNode *n = s.scene->find_node(p_path);
		REQUIRE(n != nullptr);
		REQUIRE(n->timeline != nullptr);
		std::string out;
		for (const SwfFrameLabel &l : n->timeline->labels) {
			if (l.frame <= n->frame) {
				out = l.name;
			}
		}
		return out;
	}
};

void open_scoreboard(
		Retail &r_r, ScreenRig &r_b, const HudModel &p_model = {}, std::string_view p_screen = kScoreboard32) {
	r_b.s = ui_retail::open_retail_screen(r_r, p_screen, ui_retail::retail_text_client(ui_retail::retail_text_db(r_r)));
	r_b.s.hud->set_event_sink([&r_b](const HudWidgetEvent &p_e) { r_b.events.push_back(p_e); });
	r_b.s.hud->initialize(p_model);
	r_b.s.hud->set_focus(true);
}

void open_screen(Retail &r_r, ScreenRig &r_b, std::string_view p_screen, HudClientState p_client = {}) {
	r_b.s = ui_retail::open_retail_screen(
			r_r, p_screen, ui_retail::retail_text_client(ui_retail::retail_text_db(r_r), std::move(p_client)));
	r_b.s.hud->set_event_sink([&r_b](const HudWidgetEvent &p_e) { r_b.events.push_back(p_e); });
	r_b.s.hud->initialize(HudModel{});
	r_b.s.hud->set_focus(true);
}

std::vector<As2Value> scoreboard_rows(int32_t p_count, int32_t p_local) {
	std::vector<As2Value> rows;
	for (int32_t i = 0; i < p_count; ++i) {
		As2Value r = As2Value::object();
		r.set("Index", As2Value::of(1000 + i));
		r.set("Label", As2Value::of("PLAYER " + std::to_string(i)));
		r.set("Rank", As2Value::of(0));
		r.set("Voip", As2Value::of(0));
		r.set("Kills", As2Value::of(32 - i));
		r.set("Deaths", As2Value::of(i));
		r.set("Score", As2Value::of(100 * (32 - i)));
		r.set("Ping", As2Value::of(20 + i));
		r.set("LocalSquad", As2Value::of(false));
		r.set("LocalPlayer", As2Value::of(i == p_local));
		r.set("MatchReady", As2Value::of(false));
		rows.push_back(std::move(r));
	}
	return rows;
}
}

TEST_SUITE("ui-widgets") {
	TEST_CASE("widgets: scoreboard list bindings") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		Result<HudScreenAsset> asset = read_hud_screen_asset(*r->level, kScoreboard32);
		REQUIRE_MESSAGE(asset.ok(), (asset.ok() ? std::string() : asset.error().message));
		check_scoreboard_list(*asset, "List_01", 644095251);
		check_scoreboard_list(*asset, "List_02", -163624481);
		const HudWidgetNode *header = asset->find("ScoreboardHeader_01");
		REQUIRE(header != nullptr);
		CHECK_FALSE(header->list.has_value());
		const HudDataSource *tickets = header->source("Tickets");
		REQUIRE(tickets != nullptr);
		CHECK_FALSE(tickets->use_direct_access);
	}

	TEST_CASE("widgets: scoreboard list direct access rows") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_scoreboard(*r, b);
		const HudWidgetState *list_widget = b.s.hud->widget(std::string(kHudScreenInstance) + ".List_01");
		REQUIRE(list_widget != nullptr);
		CHECK(list_widget->initialized);
		CHECK(list_widget->class_name == "Widget.List.List");
		const As2Clip list = b.clip("List_01");
		CHECK(list.visible());
		CHECK(list.x() == 103.0f);
		CHECK(list.y() == 154.0f);
		CHECK(list.xscale() == 100.0f);
		CHECK_FALSE(b.clip("List_01.ScrollBar").visible());
		for (int32_t i = 0; i < 16; ++i) {
			const As2Clip empty = b.clip("List_01.EmptyListHolder.emptyRow_" + std::to_string(i));
			REQUIRE(empty.exists());
			CHECK(empty.visible());
			CHECK(empty.y() == float(28 * i));
		}
		CHECK_FALSE(b.clip("List_01.EmptyListHolder.emptyRow_16").exists());
		REQUIRE(hud_list_set_direct_rows(*b.s.hud, "List_01", scoreboard_rows(32, 3)));
		CHECK(b.s.hud->stats().as2.missing_paths == 0);
		CHECK_MESSAGE(b.s.hud->stats().as2.failed_ops == 0, b.s.hud->stats().as2.last_failure);
		CHECK(b.s.hud->direct_access().errors().empty());
		for (int32_t i = 0; i < 16; ++i) {
			CHECK_FALSE(b.clip("List_01.EmptyListHolder.emptyRow_" + std::to_string(i)).visible());
		}
		const SwfSceneNode *holder = b.clip("List_01.ListHolder").node();
		REQUIRE(holder != nullptr);
		CHECK(holder->children.size() == 17u);
		for (int32_t i = 0; i < 17; ++i) {
			const std::string path = b.row(i);
			REQUIRE_MESSAGE(!path.empty(), i);
			const As2Clip row = b.s.hud->clip(path);
			CHECK(row.y() == float(28 * i));
			CHECK(b.s.hud->clip(path + ".Label").text() == "PLAYER " + std::to_string(i));
			CHECK(b.s.hud->clip(path + ".NumLabel").text() == std::to_string(i + 1));
			CHECK(b.s.hud->clip(path + ".KillsLabel").text() == std::to_string(32 - i));
			CHECK(b.s.hud->clip(path + ".PingLabel").text() == std::to_string(20 + i));
			CHECK(b.s.hud->clip(path + ".size_mc").width() == 526.0f);
			CHECK(b.frame_label(path) == "out");
			CHECK(b.frame_label(path + ".bg") == (i == 3 ? "outPlayer" : "out"));
			CHECK(b.s.hud->clip(path + ".Label").text_color() == (i == 3 ? 0u : 0xFFFFFFu));
		}
		const As2Clip mask = b.clip("List_01.listMask");
		CHECK(mask.width() == 536.0f);
		CHECK(mask.height() == 448.0f);
		const As2Clip bar = b.clip("List_01.ScrollBar");
		CHECK(bar.visible());
		CHECK(bar.x() == 520.0f);
		CHECK(b.clip("List_01.ScrollBar.backGround").height() == 448.0f);
		CHECK(b.clip("List_01.ScrollBar.whiteBar").height() == 224.0f);
		CHECK(b.clip("List_01.ScrollBar.whiteBar").y() == 0.0f);
		std::printf("ui.widgets scoreboard rows=%zu bar=%.0fx%.0f at x %.0f\n", holder->children.size(),
				b.clip("List_01.ScrollBar.whiteBar").width(), b.clip("List_01.ScrollBar.whiteBar").height(), bar.x());
	}

	TEST_CASE("widgets: scoreboard row hover press and release states") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_scoreboard(*r, b);
		REQUIRE(hud_list_set_direct_rows(*b.s.hud, "List_01", scoreboard_rows(32, 3)));
		const std::string row5 = b.row(5);
		REQUIRE(!row5.empty());
		const float x = 103.0f + 200.0f;
		const float y = 154.0f + 28.0f * 5.0f + 14.0f;
		CHECK(b.s.hud->hit_target(x, y) == row5);
		b.s.hud->mouse_move(x, y);
		CHECK(b.frame_label(row5) == "over");
		CHECK(b.frame_label(row5 + ".bg") == "over");
		CHECK(b.s.hud->clip(row5 + ".Label").text_color() == 0xFFFFFFu);
		b.s.hud->mouse_button(x, y, 1, true);
		CHECK(b.frame_label(row5) == "over");
		b.s.hud->mouse_button(x, y, 1, false);
		CHECK(b.frame_label(row5) == "overSelected");
		CHECK(b.frame_label(row5 + ".bg") == "overSelected");
		CHECK(b.s.hud->clip(row5 + ".Label").text_color() == 0u);
		b.s.hud->mouse_move(x, 700.0f);
		CHECK(b.frame_label(row5) == "selected");
		const std::string row6 = b.row(6);
		b.s.hud->mouse_move(x, y + 28.0f);
		CHECK(b.frame_label(row6) == "over");
		b.s.hud->mouse_button(x, y + 28.0f, 1, true);
		b.s.hud->mouse_button(x, y + 28.0f, 1, false);
		CHECK(b.frame_label(row6) == "overSelected");
		CHECK(b.frame_label(row5) == "out");
		CHECK(b.s.hud->clip(row5 + ".Label").text_color() == 0xFFFFFFu);
		const std::string row3 = b.row(3);
		b.s.hud->mouse_move(x, 154.0f + 28.0f * 3.0f + 14.0f);
		CHECK(b.frame_label(row3) == "over");
		CHECK(b.frame_label(row3 + ".bg") == "overPlayer");
	}

	TEST_CASE("widgets: scoreboard list scrolls by the scroll bar page jump") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_scoreboard(*r, b);
		REQUIRE(hud_list_set_direct_rows(*b.s.hud, "List_01", scoreboard_rows(32, 3)));
		const float x = 103.0f + 520.0f + 12.0f;
		const float y = 154.0f + 400.0f;
		CHECK(b.s.hud->hit_target(x, y) == std::string(kHudScreenInstance) + ".List_01.ScrollBar.backGround");
		b.s.hud->mouse_button(x, y, 1, true);
		b.s.hud->mouse_button(x, y, 1, false);
		for (int32_t i = 0; i < 10; ++i) {
			b.s.hud->advance(1.0 / 30.0);
		}
		CHECK(b.clip("List_01.ScrollBar.whiteBar").y() == doctest::Approx(224.0));
		CHECK(b.clip("List_01.ListHolder").y() == -448.0f);
		const std::string last = b.row(31);
		REQUIRE(!last.empty());
		CHECK(b.s.hud->clip(last + ".Label").text() == "PLAYER 31");
		CHECK(b.s.hud->clip(last + ".NumLabel").text() == "32");
	}

	TEST_CASE("widgets: scoreboard headers bind team names, kills and mode colours") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m;
		m.team.friendly_team_name = text_hash("ID_M_US");
		m.team.enemy_team1_name = text_hash("ID_M_RU");
		m.kill_counter.player_team_kills = 50;
		m.kill_counter.enemy_team_kills = 61;
		ScreenRig b;
		open_scoreboard(*r, b, m);
		CHECK(b.s.hud->stats().widgets_missing == 0);
		CHECK(b.s.hud->stats().as2.missing_paths == 0);
		CHECK_MESSAGE(b.s.hud->stats().as2.failed_ops == 0, b.s.hud->stats().as2.last_failure);
		const TextDatabase &db = ui_retail::retail_text_db(*r);
		const auto sid = [&](std::string_view p_sid) { return std::string(db.find_sid(p_sid).value_or("")); };
		struct Want {
			const char *instance;
			std::string team;
			const char *kills;
			uint32_t colour;
		};
		for (const Want &w : { Want{ "ScoreboardHeader_01", sid("ID_M_US"), "50", 0x73C3FFu },
					 Want{ "ScoreboardHeader_02", sid("ID_M_RU"), "61", 0x963232u } }) {
			const std::string h = w.instance;
			CHECK(b.clip(h).visible());
			const As2Clip team = b.clip(h + ".teamText");
			CHECK(!w.team.empty());
			CHECK(team.text() == w.team);
			const As2Clip ticket = b.clip(h + ".ticketText");
			CHECK(ticket.visible());
			CHECK(ticket.text() == w.kills);
			CHECK(ticket.x() == doctest::Approx(team.x() + team.text_width() + 25.0f));
			CHECK_FALSE(b.clip(h + ".winnerText").visible());
			CHECK_FALSE(b.clip(h + ".ObjectivesIcon").visible());
			CHECK(b.clip(h + ".killsText").text() == sid("ID_H_SCOREBOARD_KILLS"));
			CHECK(b.clip(h + ".deathsText").text() == sid("ID_H_SCOREBOARD_DEATH"));
			CHECK(b.clip(h + ".scoreText").text() == sid("ID_H_SCOREBOARD_SCORE"));
			CHECK(b.clip(h + ".pingText").text() == sid("ID_M_SB_CONNECTION"));
			CHECK(b.clip(h + ".pingText").visible());
			const As2ColorTransform bg = b.clip(h + ".bg").color_transform();
			CHECK(bg.ra == 0.0);
			CHECK(bg.ga == 0.0);
			CHECK(bg.ba == 0.0);
			CHECK(bg.aa == 100.0);
			CHECK(bg.rb == double((w.colour >> 16) & 0xffu));
			CHECK(bg.gb == double((w.colour >> 8) & 0xffu));
			CHECK(bg.bb == double(w.colour & 0xffu));
			const SwfSceneNode *label = b.clip(h + ".teamText").node();
			REQUIRE(label != nullptr);
			REQUIRE(label->filters.size() == 1u);
			CHECK(label->filters[0].kind == SwfFilterKind::Glow);
			CHECK(label->filters[0].blur_x == 7.0f);
			CHECK(label->filters[0].strength == 3.0f);
			CHECK(label->filters[0].passes == 2);
			CHECK(label->filters[0].color.r == uint8_t(w.colour >> 16));
		}
		m.kill_counter.player_team_kills = 51;
		b.s.hud->refresh(m);
		CHECK(b.clip("ScoreboardHeader_01.ticketText").text() == "51");
		CHECK(b.clip("ScoreboardHeader_02.ticketText").text() == "61");
		std::printf("ui.widgets scoreboard headers %s %s | %s %s\n",
				b.clip("ScoreboardHeader_01.teamText").text().c_str(),
				b.clip("ScoreboardHeader_01.ticketText").text().c_str(),
				b.clip("ScoreboardHeader_02.teamText").text().c_str(),
				b.clip("ScoreboardHeader_02.ticketText").text().c_str());
	}

	TEST_CASE("widgets: scoreboard header Tab release fires its event") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_scoreboard(*r, b);
		const std::string focused = b.s.hud->focused_widget();
		b.events.clear();
		b.s.hud->input_concept(int32_t(UiInputConcept::Tab), true);
		CHECK(b.events.empty());
		b.s.hud->input_concept(int32_t(UiInputConcept::Tab), false);
		REQUIRE(b.events.size() == 1u);
		CHECK(focused == std::string(kHudScreenInstance) + ".ScoreboardHeader_01");
		CHECK(b.events[0].instance == "ScoreboardHeader_01");
		CHECK(b.events[0].query == "UIWidgetEventID_OnItemReleased");
	}

	TEST_CASE("widgets: scoreboard spectator box joins names as the AS2 does") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_scoreboard(*r, b, HudModel{}, kScoreboardTwoTeams);
		CHECK(b.s.hud->stats().widgets_missing == 0);
		const std::string path = std::string(kHudScreenInstance) + ".ScoreboardSpectatorBox_01";
		HudWidgetController *box = b.s.hud->controller(path);
		REQUIRE(box != nullptr);
		HudWidgetHost *host = b.s.hud->host(path);
		REQUIRE(host != nullptr);
		CHECK_FALSE(b.clip("ScoreboardSpectatorBox_01").visible());
		As2Value one = As2Value::array();
		one.push(As2Value::object().set("Name", As2Value::of("ALPHA")));
		box->update(*host, "Spectators", one);
		CHECK(b.clip("ScoreboardSpectatorBox_01").visible());
		CHECK(b.clip("ScoreboardSpectatorBox_01.Label0").text() == "ALPHA");
		one.push(As2Value::object().set("Name", As2Value::of("BRAVO")));
		box->update(*host, "Spectators", one);
		CHECK(b.clip("ScoreboardSpectatorBox_01.Label0").text() == ", BRAVO");
		box->update(*host, "Spectators", As2Value::array());
		CHECK_FALSE(b.clip("ScoreboardSpectatorBox_01").visible());
		CHECK(b.clip("ScoreboardSpectatorBox_01.Label0").text() == ", BRAVO");
	}

	TEST_CASE("widgets: spawn screen tab bar lays out its three tabs and selects on click") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_screen(*r, b, kSpawnScreenPc);
		const TextDatabase &db = ui_retail::retail_text_db(*r);
		const std::string bar = std::string(kHudScreenInstance) + ".TabBar_01";
		const HudWidgetState *w = b.s.hud->widget(bar);
		REQUIRE(w != nullptr);
		CHECK(w->initialized);
		CHECK(w->class_name == "Widget.Button.TabBar");
		CHECK(b.clip("TabBar_01").visible());
		CHECK_FALSE(b.clip("TabBar_01.tabBarSize").visible());
		const float bar_width = b.clip("TabBar_01.tabBarSize").width();
		const std::array<std::string_view, 3> sids{ "ID_M_TAB_SPAWN", "ID_M_TAB_SQUAD", "ID_M_TAB_CUSTOMIZE" };
		float next_x = 0.0f;
		std::array<float, 3> centre_x{};
		const float row_y = b.clip("TabBar_01").y() + 17.0f;
		for (size_t i = 0; i < sids.size(); ++i) {
			const std::string tab = "TabBar_01.tabBarHolder.button_" + std::to_string(i);
			const As2Clip mc = b.clip(tab);
			REQUIRE(mc.exists());
			CHECK(b.clip(tab + ".button.label.txt").text() == std::string(db.find_sid(sids[i]).value_or("?")));
			CHECK(mc.x() == doctest::Approx(next_x));
			const float width = b.clip(tab + ".button.bgHolder").width() - 2.0f;
			next_x = mc.x() + width + 2.0f;
			centre_x[i] = b.clip("TabBar_01").x() + mc.x() + width * 0.5f;
		}
		CHECK(next_x - 2.0f == doctest::Approx(bar_width).epsilon(0.01));
		CHECK(b.clip("TabBar_01.tabBarHolder.button_0.button.bgHolder.selected_mc").visible());
		CHECK(b.clip("TabBar_01.tabBarHolder.button_0.button.labelInvert").visible());
		CHECK_FALSE(b.clip("TabBar_01.tabBarHolder.button_2.button.bgHolder.selected_mc").visible());
		const std::string tab2 = bar + ".tabBarHolder.button_2";
		CHECK(b.s.hud->hit_target(centre_x[2], row_y) == tab2);
		b.events.clear();
		b.s.hud->mouse_move(centre_x[2], row_y);
		REQUIRE_FALSE(b.events.empty());
		CHECK(b.events.back().instance == "TabBar_01");
		CHECK(b.events.back().index == 2);
		CHECK(b.events.back().param.to_string() == "ID2");
		CHECK(b.clip("TabBar_01.tabBarHolder.button_2.button.bgHolder.over_mc").visible());
		b.s.hud->mouse_button(centre_x[2], row_y, 1, true);
		CHECK(b.events.back().index == 0);
		b.s.hud->mouse_button(centre_x[2], row_y, 1, false);
		CHECK(b.events.back().index == 1);
		CHECK(b.events.back().param.to_string() == "ID2");
		CHECK(b.clip("TabBar_01.tabBarHolder.button_2.button.bgHolder.selected_mc").visible());
		CHECK_FALSE(b.clip("TabBar_01.tabBarHolder.button_0.button.bgHolder.selected_mc").visible());
		CHECK(b.clip("TabBar_01.tabBarHolder.button_0.button.label").visible());
		const size_t before = b.events_of("TabBar_01");
		b.s.hud->input_concept(int32_t(UiInputConcept::TabLeft), true);
		b.s.hud->input_concept(int32_t(UiInputConcept::TabLeft), false);
		CHECK(b.events_of("TabBar_01") == before);
		HudWidgetController *c = b.s.hud->controller(bar);
		REQUIRE(c != nullptr);
		c->update(*b.s.hud->host(bar), "SelectedTab", As2Value::of(1));
		CHECK(b.clip("TabBar_01.tabBarHolder.button_1.button.bgHolder.selected_mc").visible());
		CHECK_FALSE(b.clip("TabBar_01.tabBarHolder.button_2.button.bgHolder.selected_mc").visible());
		std::printf("ui.widgets tab bar %.0f px: %s %.0f | %s %.0f | %s %.0f\n", bar_width,
				b.clip("TabBar_01.tabBarHolder.button_0.button.label.txt").text().c_str(),
				b.clip("TabBar_01.tabBarHolder.button_0").x(),
				b.clip("TabBar_01.tabBarHolder.button_1.button.label.txt").text().c_str(),
				b.clip("TabBar_01.tabBarHolder.button_1").x(),
				b.clip("TabBar_01.tabBarHolder.button_2.button.label.txt").text().c_str(),
				b.clip("TabBar_01.tabBarHolder.button_2").x());
	}

	TEST_CASE("widgets: console tab bar steps tabs on TabLeft and TabRight releases") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudClientState console;
		console.platform = "ps3";
		ScreenRig b;
		open_screen(*r, b, kSpawnScreenPc, console);
		const std::string bar = std::string(kHudScreenInstance) + ".TabBar_01";
		b.s.hud->focus_widget(bar);
		REQUIRE(b.s.hud->focused_widget() == bar);
		CHECK(b.clip("TabBar_01.tabBarHolder.btnIcon1").exists());
		CHECK(b.clip("TabBar_01.tabBarHolder.btnIcon2").exists());
		CHECK(b.clip("TabBar_01.tabBarHolder.button_0").x() > 0.0f);
		b.events.clear();
		b.s.hud->input_concept(int32_t(UiInputConcept::TabRight), true);
		CHECK(b.events_of("TabBar_01") == 0u);
		b.s.hud->input_concept(int32_t(UiInputConcept::TabRight), false);
		REQUIRE_FALSE(b.events.empty());
		CHECK(b.events.back().index == 1);
		CHECK(b.events.back().param.to_string() == "ID1");
		CHECK(b.clip("TabBar_01.tabBarHolder.button_1.button.bgHolder.selected_mc").visible());
		b.s.hud->input_concept(int32_t(UiInputConcept::TabLeft), false);
		b.s.hud->input_concept(int32_t(UiInputConcept::TabLeft), false);
		CHECK(b.events.back().param.to_string() == "ID2");
		CHECK(b.clip("TabBar_01.tabBarHolder.button_2.button.bgHolder.selected_mc").visible());
		CHECK_FALSE(b.clip("TabBar_01.tabBarHolder.button_0.button.bgHolder.selected_mc").visible());
	}

	TEST_CASE("widgets: generic popup shows Save settings with YES and NO") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const TextDatabase &db = ui_retail::retail_text_db(*r);
		HudPopup popup;
		popup.title = std::string(db.find(kOptionsTitleHash).value_or(""));
		popup.text = std::string(db.find(kSaveSettingsHash).value_or(""));
		popup.buttons = { { "ID_M_POPUP_YES", -1 }, { "ID_M_POPUP_NO", -1 } };
		REQUIRE(popup.title == "Options");
		REQUIRE(popup.text == "Save settings?");
		HudClientState client;
		client.popup = &popup;
		ScreenRig b;
		open_screen(*r, b, kPopupGeneric, client);
		CHECK(b.s.hud->stats().widgets_missing == 0);
		CHECK(b.s.hud->stats().as2.missing_paths == 0);
		CHECK_MESSAGE(b.s.hud->stats().as2.failed_ops == 0, b.s.hud->stats().as2.last_failure);
		const As2Clip screen = b.s.hud->clip(kHudScreenInstance);
		CHECK(screen.xscale() == doctest::Approx(80.0));
		CHECK(screen.alpha() == doctest::Approx(0.0));
		CHECK(screen.x() == doctest::Approx(128.0));
		CHECK(screen.y() == doctest::Approx(72.0));
		b.s.hud->advance(0.2);
		CHECK(screen.xscale() > 95.0f);
		CHECK(screen.xscale() < 100.0f);
		b.s.hud->advance(0.25);
		CHECK(screen.xscale() == doctest::Approx(100.0));
		CHECK(screen.yscale() == doctest::Approx(100.0));
		CHECK(screen.alpha() == doctest::Approx(100.0));
		CHECK(screen.x() == doctest::Approx(0.0));
		CHECK(screen.y() == doctest::Approx(0.0));
		CHECK(b.s.hud->enter_complete());
		CHECK(b.clip("PopupGenericWidget_01").visible());
		CHECK(b.clip("PopupGenericWidget_01.popupTitle").text() == "Options");
		CHECK(b.clip("PopupGenericWidget_01.popupText").text() == "Save settings?");
		std::string box;
		for (const SwfNodeId id : b.clip("PopupGenericWidget_01").node()->children) {
			const SwfSceneNode *c = b.s.scene->node(id);
			if (c != nullptr && b.s.scene->find_node(c->path + ".header") != nullptr) {
				box = c->path;
			}
		}
		REQUIRE_FALSE(box.empty());
		const As2Clip popup_box = b.s.hud->clip(box);
		CHECK(popup_box.xscale() == 100.0f);
		CHECK(popup_box.yscale() == 100.0f);
		CHECK(b.s.hud->clip(box + ".icon").current_frame() == 2u);
		CHECK(b.s.hud->clip(box + ".header").width() == doctest::Approx(609.93).epsilon(1e-4));
		CHECK(b.s.hud->clip(box + ".header").height() == doctest::Approx(34.0));
		CHECK(b.s.hud->clip(box + ".bg").width() == doctest::Approx(609.93).epsilon(1e-4));
		CHECK(b.s.hud->clip(box + ".bg").height() == doctest::Approx(265.99).epsilon(1e-4));
		CHECK(b.s.hud->clip(box + ".icon").x() == doctest::Approx(564.93).epsilon(1e-4));
		std::printf("ui.widgets popup box %.2f x %.2f, header %.2f x %.2f, bg y %.2f h %.2f, icon x %.2f\n",
				popup_box.width(), popup_box.height(), b.s.hud->clip(box + ".header").width(),
				b.s.hud->clip(box + ".header").height(), b.s.hud->clip(box + ".bg").y(),
				b.s.hud->clip(box + ".bg").height(), b.s.hud->clip(box + ".icon").x());
		const As2Clip dimmer = b.clip("Dimmer_01");
		CHECK(dimmer.visible());
		CHECK(dimmer.width() == doctest::Approx(1280.0 + 256.0));
		CHECK(dimmer.height() == doctest::Approx(720.0 + 144.0));
		CHECK(b.clip("Dimmer_01.dimmer").visible());
		const As2Clip manager = b.clip("PopupButtonsManager_01");
		CHECK(manager.visible());
		CHECK(manager.xscale() == doctest::Approx(100.0));
		CHECK_FALSE(b.clip("PopupButtonsManager_01.placeholder").visible());
		const As2Clip holder = b.clip("PopupButtonsManager_01.holder");
		const As2Clip yes = b.clip("PopupButtonsManager_01.holder.btn0");
		const As2Clip no = b.clip("PopupButtonsManager_01.holder.btn1");
		REQUIRE(yes.exists());
		REQUIRE(no.exists());
		CHECK(b.clip("PopupButtonsManager_01.holder.btn0.button.label.txt").text() == "YES");
		CHECK(b.clip("PopupButtonsManager_01.holder.btn1.button.label.txt").text() == "NO");
		const float yes_width = b.clip("PopupButtonsManager_01.holder.btn0.button.bgHolder").width() - 2.0f;
		CHECK(yes.x() == 0.0f);
		CHECK(no.x() == doctest::Approx(yes_width + 12.0f));
		CHECK(b.clip("PopupButtonsManager_01.holder.btn0.button.bgHolder.selected_mc").visible());
		CHECK(b.clip("PopupButtonsManager_01.holder.btn0.button.labelInvert").visible());
		CHECK_FALSE(b.clip("PopupButtonsManager_01.holder.btn1.button.bgHolder.selected_mc").visible());
		const float yes_left = manager.x() + holder.x() + yes.x();
		const float no_left = manager.x() + holder.x() + no.x();
		std::printf("ui.widgets popup buttons YES x %.1f w %.1f, NO x %.1f, row y %.1f, holder x %.1f\n", yes_left,
				yes_width, no_left, manager.y(), holder.x());
		const std::string mgr = std::string(kHudScreenInstance) + ".PopupButtonsManager_01";
		b.s.hud->focus_widget(mgr);
		b.events.clear();
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateRight), true);
		CHECK(b.clip("PopupButtonsManager_01.holder.btn1.button.bgHolder.selected_mc").visible());
		CHECK_FALSE(b.clip("PopupButtonsManager_01.holder.btn0.button.bgHolder.selected_mc").visible());
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateRight), true);
		CHECK(b.clip("PopupButtonsManager_01.holder.btn1.button.bgHolder.selected_mc").visible());
		b.s.hud->input_concept(int32_t(UiInputConcept::Activate), true);
		REQUIRE_FALSE(b.events.empty());
		CHECK(b.events.back().instance == "PopupButtonsManager_01");
		CHECK(b.events.back().index == 0);
		CHECK(b.events.back().param.to_string() == "ID_M_POPUP_NO");
		const float click_x = yes_left + yes_width * 0.5f;
		const float click_y = manager.y() + 10.0f;
		CHECK(b.s.hud->hit_target(click_x, click_y) == mgr + ".holder.btn0");
		b.events.clear();
		b.s.hud->mouse_move(click_x, click_y);
		b.s.hud->mouse_button(click_x, click_y, 1, true);
		b.s.hud->mouse_button(click_x, click_y, 1, false);
		REQUIRE(b.events.size() == 2u);
		CHECK(b.events[0].query == "UIWidgetEventID_PlaySoundSelect");
		CHECK(b.events[1].index == 0);
		CHECK(b.events[1].param.to_string() == "ID_M_POPUP_YES");
		CHECK(b.s.hud->hit_target(100.0f, 100.0f) == std::string(kHudScreenInstance) + ".Dimmer_01");
	}

	TEST_CASE("widgets: list label follows its row's state") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_scoreboard(*r, b);
		REQUIRE(hud_list_set_direct_rows(*b.s.hud, "List_01", scoreboard_rows(32, 3)));
		const std::string row5 = b.row(5);
		REQUIRE(!row5.empty());
		const As2Clip row_clip = b.s.hud->clip(row5);
		HudWidgetHost *row_host = b.s.hud->host(row5);
		REQUIRE(row_host != nullptr);
		const As2Clip lbl = row_host->attach_movie(row_clip, "mc_label", "lbl", row_clip.next_highest_depth());
		REQUIRE(lbl.exists());
		const HudWidgetState *state = b.s.hud->widget(lbl.path());
		REQUIRE(state != nullptr);
		CHECK(state->class_name == "Widget.List.ListLabel");
		HudListLabel *label = dynamic_cast<HudListLabel *>(b.s.hud->controller(lbl.path()));
		REQUIRE(label != nullptr);
		const As2Clip txt = lbl.child("txtField");
		REQUIRE(txt.exists());
		CHECK(label->text() == "LABEL");
		CHECK(txt.text() == "LABEL");
		REQUIRE(txt.node()->edit_text != nullptr);
		CHECK(txt.node()->edit_text->id == 318);
		CHECK(txt.node()->edit_text->height == 22.0f);
		CHECK(label->type() == "medium");
		CHECK(label->label_state() == "standard");
		CHECK(txt.text_color() == 0xFFFFFFu);
		const auto glow = [&txt]() {
			const SwfSceneNode *n = txt.node();
			REQUIRE(n != nullptr);
			REQUIRE(n->filters.size() == 1u);
			return n->filters[0];
		};
		CHECK(glow().color.r == 0x73);
		CHECK(glow().color.g == 0xC3);
		CHECK(glow().color.b == 0xFF);
		CHECK(glow().blur_x == 8.0f);
		CHECK(glow().strength == 1.0f);
		label->set_text("ASSAULT");
		CHECK(txt.text() == "ASSAULT");
		CHECK(txt.height() == doctest::Approx(txt.text_height()));
		const float x = 103.0f + 200.0f;
		const float y = 154.0f + 28.0f * 5.0f + 14.0f;
		b.s.hud->mouse_move(x, y);
		CHECK(label->label_state() == "standard");
		b.s.hud->mouse_button(x, y, 1, true);
		b.s.hud->mouse_button(x, y, 1, false);
		CHECK(b.frame_label(row5) == "overSelected");
		CHECK(label->label_state() == "inverted");
		CHECK(txt.text_color() == 0u);
		CHECK(glow().color.r == 0);
		CHECK(glow().color.b == 0);
		CHECK(glow().strength == 0.75f);
		CHECK(txt.alpha() == doctest::Approx(100.0));
		b.s.hud->mouse_move(x, y + 28.0f);
		b.s.hud->mouse_button(x, y + 28.0f, 1, true);
		b.s.hud->mouse_button(x, y + 28.0f, 1, false);
		CHECK(b.frame_label(row5) == "out");
		CHECK(label->label_state() == "standard");
		CHECK(txt.text_color() == 0xFFFFFFu);
		RowBaseClass *row = dynamic_cast<RowBaseClass *>(b.s.hud->controller(row5));
		REQUIRE(row != nullptr);
		row->set_disabled(true);
		CHECK(label->label_state() == "inactive");
		CHECK(txt.alpha() == doctest::Approx(60.0));
		CHECK(txt.text_color() == 0u);
		row->set_disabled(false);
		label->set_interactive(false);
		b.s.hud->mouse_move(x, y);
		b.s.hud->mouse_button(x, y, 1, true);
		b.s.hud->mouse_button(x, y, 1, false);
		CHECK(b.frame_label(row5) == "overSelected");
		CHECK(label->label_state() == "standard");
		CHECK(txt.text_color() == 0xFFFFFFu);
		label->set_type("Bold");
		CHECK(label->type() == "bold");
		label->set_scrollable(true);
		label->set_label_width(40.0f);
		label->set_text("A LONG LIST LABEL THAT SCROLLS");
		const As2Clip scrolled = lbl.child("txtField");
		const float tw = scrolled.text_width();
		REQUIRE(tw > 40.0f);
		CHECK(lbl.child("mask").exists());
		CHECK(scrolled.width() == doctest::Approx(tw + 5.0f));
		CHECK(scrolled.x() == 0.0f);
		b.s.hud->advance(2.5);
		b.s.hud->advance(2.0);
		CHECK(scrolled.x() == doctest::Approx(-(tw - 40.0f + 5.0f)).epsilon(0.01));
		CHECK(b.s.hud->stats().as2.missing_paths == 0);
		std::printf("ui.widgets list label %.1f px text, scrolled to x %.1f\n", tw, scrolled.x());
	}

	TEST_CASE("widgets: page headers show unless hidden on PC") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig menu;
		open_screen(*r, menu, "ui/flow/screen/ingamemenump");
		const HudWidgetState *w = menu.s.hud->widget(std::string(kHudScreenInstance) + ".PageHeader_01");
		REQUIRE(w != nullptr);
		CHECK(w->class_name == "Widget.Text.PageHeader");
		CHECK(w->initialized);
		CHECK(menu.clip("PageHeader_01").visible());
		const SwfSceneNode *heading = menu.clip("PageHeader_01.heading").node();
		REQUIRE(heading != nullptr);
		CHECK(heading->text_options.auto_size != SwfTextAutoSize::None);
		CHECK(menu.clip("PageHeader_01.subHeading").node()->text_options.auto_size != SwfTextAutoSize::None);
		ScreenRig squad;
		open_screen(*r, squad, "ui/flow/screen/squadscreen");
		CHECK_FALSE(squad.clip("PageHeader_01").visible());
		std::printf("ui.widgets page header '%s' / '%s'\n", menu.clip("PageHeader_01.heading").text().c_str(),
				menu.clip("PageHeader_01.subHeading").text().c_str());
	}

	TEST_CASE("widgets: text fields attach their row type and take text") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_screen(*r, b, "ui/flow/screen/eorprogressscreen");
		CHECK(b.s.hud->stats().as2.missing_paths == 0);
		const std::string path = std::string(kHudScreenInstance) + ".TextField_02";
		const HudWidgetState *w = b.s.hud->widget(path);
		REQUIRE(w != nullptr);
		CHECK(w->class_name == "Widget.Text.TextField");
		const As2Value *row_type = b.s.hud->host(path)->construct_param("m_rowType");
		REQUIRE(row_type != nullptr);
		const As2Clip field = b.clip("TextField_02." + row_type->to_string());
		REQUIRE(field.exists());
		CHECK(field.x() == 0.0f);
		CHECK(field.y() == 0.0f);
		const As2Clip self = b.clip("TextField_02");
		CHECK(self.xscale() == 100.0f);
		CHECK(self.yscale() == 100.0f);
		const As2Clip display = field.child("txtDisplay");
		REQUIRE(display.exists());
		CHECK(display.text() == "NEXT ROUND STARTS IN:");
		HudWidgetController *c = b.s.hud->controller(path);
		HudWidgetHost *h = b.s.hud->host(path);
		c->update(*h, "Text", As2Value::of("ROUND OVER"));
		CHECK(display.text() == "ROUND OVER");
		c->update(*h, "Text", As2Value{});
		CHECK(display.text().empty());
		c->in_event(*h, 1, As2Value::of(0));
		CHECK_FALSE(self.visible());
		c->in_event(*h, 0, As2Value::of(0));
		CHECK(self.visible());
		c->update(*h, "Visible", As2Value::of(false));
		CHECK_FALSE(self.visible());
		std::printf("ui.widgets text field %s %.0fx%.0f\n", row_type->to_string().c_str(), display.width(),
				display.height());
		ScreenRig chat;
		open_screen(*r, chat, "ui/flow/screen/chat/chatlogscreen");
		const std::string chat_path = std::string(kHudScreenInstance) + ".TextField_01";
		const As2Value *chat_type = chat.s.hud->host(chat_path)->construct_param("m_rowType");
		REQUIRE(chat_type != nullptr);
		CHECK(chat_type->to_string() == "ChatLogMode");
		const As2Clip chat_self = chat.clip("TextField_01");
		chat_self.set_visible(false);
		chat.s.hud->controller(chat_path)->update(*chat.s.hud->host(chat_path), "Text", As2Value::of("hello"));
		CHECK(chat_self.visible());
		chat.s.hud->advance(3.9);
		CHECK(chat_self.visible());
		chat.s.hud->advance(0.2);
		CHECK_FALSE(chat_self.visible());
	}

	TEST_CASE("widgets: input listener fires Tab and Com events") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_screen(*r, b, "ui/flow/screen/tablistenerscreen");
		const std::string path = std::string(kHudScreenInstance) + ".InputListener_01";
		const HudWidgetState *w = b.s.hud->widget(path);
		REQUIRE(w != nullptr);
		CHECK(w->class_name == "Widget.Input.InputListener");
		CHECK_FALSE(b.clip("InputListener_01").visible());
		b.s.hud->focus_widget(path);
		b.s.hud->input_concept(int32_t(UiInputConcept::Tab), true);
		REQUIRE(b.events.size() == 1u);
		CHECK(b.events[0].index == 1);
		CHECK(b.events[0].query == "UIWidgetEventID_OnItemPressed");
		b.s.hud->input_concept(int32_t(UiInputConcept::Tab), false);
		REQUIRE(b.events.size() == 2u);
		CHECK(b.events[1].index == 2);
		CHECK(b.events[1].query == "UIWidgetEventID_OnItemReleased");
		b.s.hud->input_concept(int32_t(UiInputConcept::Com), true);
		CHECK(b.events.size() == 2u);
		b.s.hud->advance(0.1);
		CHECK(b.events.size() == 2u);
		b.s.hud->advance(0.15);
		REQUIRE(b.events.size() == 3u);
		CHECK(b.events[2].index == 3);
		CHECK(b.events[2].query == "UIWidgetEventID_OnComPressed");
		b.s.hud->advance(1.0);
		CHECK(b.events.size() == 3u);
		b.s.hud->input_concept(int32_t(UiInputConcept::Com), false);
		REQUIRE(b.events.size() == 4u);
		CHECK(b.events[3].index == 4);
		CHECK(b.events[3].query == "UIWidgetEventID_OnComReleased");
	}

	TEST_CASE("widgets: console button bar stays hidden on PC and lays out pushed buttons") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		ScreenRig b;
		open_screen(*r, b, "ui/flow/screen/ingamemenump");
		const std::string path = std::string(kHudScreenInstance) + ".ConsoleButtonBar_01";
		const HudWidgetState *w = b.s.hud->widget(path);
		REQUIRE(w != nullptr);
		CHECK(w->class_name == "Widget.Button.ConsoleButtonBar");
		CHECK(w->initialized);
		const As2Clip bar = b.clip("ConsoleButtonBar_01");
		CHECK_FALSE(bar.visible());
		CHECK_FALSE(b.clip("ConsoleButtonBar_01.layoutGuide").visible());
		b.s.hud->focus_widget(path);
		CHECK(std::any_of(b.events.begin(), b.events.end(),
				[](const HudWidgetEvent &e) { return e.query == "UIWidgetEventID_OnMapChanged"; }));
		for (const HudWidgetEvent &e : b.events) {
			CHECK(e.instance == "List_01");
		}
		b.events.clear();
		b.s.hud->input_concept(int32_t(UiInputConcept::Activate), true);
		CHECK(b.events.empty());
		REQUIRE(b.s.hud->deliver_in_event("ConsoleButtonBar_01", "UIWidgetEventID_Update",
				As2Value::of("ID_M_SELECT,IDB_Rdown,false,0;ID_M_BACK,IDB_Rright,true,0")));
		const As2Clip select = b.clip("ConsoleButtonBar_01.button_IDB_Rdown");
		const As2Clip back = b.clip("ConsoleButtonBar_01.button_IDB_Rright");
		REQUIRE(select.exists());
		REQUIRE(back.exists());
		CHECK_FALSE(bar.visible());
		CHECK(select.x() == 0.0f);
		const As2Clip icon = select.child("btnIcon");
		const As2Clip label = select.child("btnLabel");
		CHECK(back.x() == doctest::Approx(icon.width() + 5.0f + label.text_width() + 15.0f));
		CHECK(label.x() == doctest::Approx(std::floor(icon.width() + 0.5f)));
		CHECK(label.text() == select.child("btnLabelBg").text());
		CHECK(back.alpha() == doctest::Approx(25.0));
		b.s.hud->input_concept(int32_t(UiInputConcept::Activate), true);
		REQUIRE(b.events.size() == 1u);
		CHECK(b.events[0].index == 0);
		CHECK(b.events[0].param.to_string() == "ID_M_SELECT");
		b.s.hud->input_concept(int32_t(UiInputConcept::Deactivate), true);
		CHECK(b.events.size() == 1u);
		REQUIRE(b.s.hud->deliver_in_event("ConsoleButtonBar_01", "UIWidgetEventID_Show"));
		CHECK(bar.visible());
		REQUIRE(b.s.hud->deliver_in_event("ConsoleButtonBar_01", "UIWidgetEventID_Hide"));
		CHECK_FALSE(bar.visible());
		CHECK(b.s.hud->stats().as2.missing_paths == 0);
		std::printf("ui.widgets console button bar: SELECT '%s' at 0, BACK at %.1f\n", label.text().c_str(), back.x());
	}

	TEST_CASE("widgets: install unchanged") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const SnapshotDiff diff = diff_snapshots(r->mount->before, r->mount->reader.snapshot());
		std::printf("ui.widgets install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
