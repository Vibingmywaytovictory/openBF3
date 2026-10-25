#include <doctest/doctest.h>

#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/ui_direct_access.h"
#include "test_support.h"
#include "ui_retail_support.h"

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

struct Scoreboard {
	ui_retail::RetailScreen s;
	std::vector<HudWidgetEvent> events;

	As2Clip clip(std::string_view p_path) const {
		return s.hud->clip(std::string(kHudScreenInstance) + "." + std::string(p_path));
	}
	std::string row(int32_t p_index) const {
		const As2Clip holder = clip("List_01.ListHolder");
		const SwfSceneNode *n = holder.node();
		REQUIRE(n != nullptr);
		for (const SwfNodeId id : n->children) {
			const SwfSceneNode *c = s.scene->node(id);
			if (c != nullptr && c->visible && std::lround(c->matrix.ty) == std::lround(28.0 * p_index)) {
				return c->path;
			}
		}
		return {};
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

void open_scoreboard(Retail &r_r, Scoreboard &r_b) {
	r_b.s = ui_retail::open_retail_screen(
			r_r, kScoreboard32, ui_retail::retail_text_client(ui_retail::retail_text_db(r_r)));
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
		Scoreboard b;
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
		Scoreboard b;
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
		Scoreboard b;
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
