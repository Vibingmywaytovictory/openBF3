#include <doctest/doctest.h>

#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/hud_screen.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <cstdio>
#include <string>
#include <string_view>

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
