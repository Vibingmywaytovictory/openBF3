#include <doctest/doctest.h>

#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/ui/ui_input.h"
#include "list.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using ui_retail::Retail;
using ui_retail::retail_or_skip;

namespace {

constexpr std::string_view kIngameMenu = "ui/flow/screen/ingamemenump";
constexpr float kMenuListX = 96.0f;
constexpr float kMenuListY = 96.0f;
constexpr float kMenuRowPitch = 36.0f;
constexpr float kMenuLabelTextSize = 30.0f;

struct MenuRig {
	ui_retail::RetailScreen s;
	std::vector<HudWidgetEvent> events;

	As2Clip clip(std::string_view p_path) const {
		return s.hud->clip(std::string(kHudScreenInstance) + "." + std::string(p_path));
	}
	std::vector<std::string> rows() const {
		std::vector<std::pair<float, std::string>> found;
		const SwfSceneNode *n = clip("List_01.ListHolder").node();
		REQUIRE(n != nullptr);
		for (const SwfNodeId id : n->children) {
			const SwfSceneNode *c = s.scene->node(id);
			if (c != nullptr && c->visible && c->kind == SwfNodeKind::Sprite) {
				found.emplace_back(c->matrix.ty, c->path);
			}
		}
		std::sort(found.begin(), found.end());
		std::vector<std::string> out;
		for (const auto &f : found) {
			out.push_back(f.second);
		}
		return out;
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
	std::vector<HudWidgetEvent> released() const {
		std::vector<HudWidgetEvent> out;
		for (const HudWidgetEvent &e : events) {
			if (e.query == "UIWidgetEventID_OnItemReleased") {
				out.push_back(e);
			}
		}
		return out;
	}
};

HudModel menu_model(bool p_suicide) {
	HudModel m;
	HudMenuList &list = m.ingame_menu.menu_entries;
	for (size_t i = 0; i < kHudMenuActionSids.size(); ++i) {
		HudMenuEntry &e = list.entries[list.count++];
		e.label_sid_hash = text_hash(kHudMenuActionSids[i]);
		e.action = HudMenuAction(i);
		e.enabled = e.action != HudMenuAction::Squad && e.action != HudMenuAction::Options &&
				(e.action != HudMenuAction::Suicide || p_suicide);
	}
	m.user.is_in_menu = true;
	constexpr std::string_view kMode = "TeamDeathMatch0";
	std::copy(kMode.begin(), kMode.end(), m.settings.current_game_mode.begin());
	m.settings.current_game_mode_name = text_hash("ID_M_GAMEMODE_TEAMDEATHMATCH");
	m.level.current_level = text_hash("ID_M_LEVELNAME_MP_SUBWAY");
	m.team.friendly_team_name = text_hash("ID_M_US");
	return m;
}

void open_menu(Retail &r_r, MenuRig &r_b, const HudModel &p_model) {
	r_b.s = ui_retail::open_retail_screen(
			r_r, kIngameMenu, ui_retail::retail_text_client(ui_retail::retail_text_db(r_r)));
	r_b.s.hud->set_event_sink([&r_b](const HudWidgetEvent &p_e) { r_b.events.push_back(p_e); });
	r_b.s.hud->initialize(p_model);
	r_b.s.hud->set_focus(true);
	r_b.s.hud->refresh(p_model);
}

}

TEST_SUITE("ui-menu") {
	TEST_CASE("menu: the game menu list binds the menu entries as MenuRow rows") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		MenuRig b;
		open_menu(*r, b, menu_model(true));
		const HudWidgetNode *node = b.s.asset.find("List_01");
		REQUIRE(node != nullptr);
		REQUIRE(node->list.has_value());
		CHECK(node->list->row_type == "MenuRow_34px");
		CHECK(node->list->row_spacing == 2);
		CHECK(node->list->navigation_type == "loop");
		CHECK(node->list->list_query.key == 366660527);
		CHECK(node->list->default_highlighted_row == 0);
		const std::vector<std::string> rows = b.rows();
		REQUIRE(rows.size() == kHudMenuActionSids.size());
		constexpr std::string_view kLabels[] = { "RESUME", "SCOREBOARD", "SQUAD & TEAM", "SUICIDE", "OPTIONS", "QUIT" };
		for (size_t i = 0; i < rows.size(); ++i) {
			const As2Clip row = b.s.hud->clip(rows[i]);
			CHECK(row.y() == doctest::Approx(kMenuRowPitch * float(i)));
			const As2Clip field = b.s.hud->clip(rows[i] + ".Label.txtField");
			REQUIRE(field.exists());
			CHECK(field.text() == kLabels[i]);
			const HudWidgetState *w = b.s.hud->widget(rows[i]);
			REQUIRE(w != nullptr);
			CHECK(w->class_name == "Widget.List.Rows.MenuRow");
		}
		CHECK(b.frame_label(rows[0]) == "selected");
		CHECK(b.s.hud->clip(rows[0] + ".Label.txtField").text_color() == 0u);
		CHECK(b.frame_label(rows[1]) == "out");
		CHECK(b.s.hud->clip(rows[1] + ".Label.txtField").text_color() == 0xFFFFFFu);
		CHECK(b.frame_label(rows[2]) == "inactive");
		CHECK(b.s.hud->clip(rows[2] + ".Label.txtField").text_color() == 0x999999u);
		CHECK_FALSE(b.clip("List_01.dummyTextField").exists());
		CHECK(b.s.hud->stats().as2.missing_paths == 0);
		CHECK_MESSAGE(b.s.hud->stats().as2.failed_ops == 0, b.s.hud->stats().as2.last_failure);
		const std::optional<float> label_size = b.s.hud->clip(rows[0] + ".Label.txtField").text_size();
		REQUIRE(label_size.has_value());
		CHECK(*label_size == kMenuLabelTextSize);
		std::printf("ui.menu rows %zu, row 0 at %.1f,%.1f width %.1f, label text size %.1f\n", rows.size(),
				b.s.hud->clip(rows[0]).x(), b.s.hud->clip(rows[0]).y(), b.s.hud->clip(rows[0] + ".bg").width(),
				*label_size);
	}

	TEST_CASE("menu: keys step the highlight past the disabled row and Enter releases the label") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		MenuRig b;
		open_menu(*r, b, menu_model(true));
		const std::vector<std::string> rows = b.rows();
		REQUIRE(rows.size() == kHudMenuActionSids.size());
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateDown), true);
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateDown), false);
		CHECK(b.frame_label(rows[1]) == "selected");
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateDown), true);
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateDown), false);
		CHECK(b.frame_label(rows[2]) == "inactive");
		CHECK(b.frame_label(rows[3]) == "selected");
		b.events.clear();
		b.s.hud->input_concept(int32_t(UiInputConcept::Activate), true);
		b.s.hud->input_concept(int32_t(UiInputConcept::Activate), false);
		const std::vector<HudWidgetEvent> out = b.released();
		REQUIRE(out.size() == 1);
		CHECK(out[0].instance == "List_01");
		CHECK(out[0].param.to_string() == "ID_M_IGMMP_SUICIDE");
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateUp), true);
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateUp), true);
		CHECK(b.frame_label(rows[0]) == "selected");
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateUp), true);
		CHECK(b.frame_label(rows[5]) == "selected");
		b.s.hud->input_concept(int32_t(UiInputConcept::NavigateUp), true);
		CHECK(b.frame_label(rows[4]) == "inactive");
		CHECK(b.frame_label(rows[3]) == "selected");
	}

	TEST_CASE("menu: the mouse hovers a row grey and a click releases it") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		MenuRig b;
		open_menu(*r, b, menu_model(true));
		const std::vector<std::string> rows = b.rows();
		REQUIRE(rows.size() == kHudMenuActionSids.size());
		const float x = kMenuListX + 120.0f;
		const auto y_of = [](size_t p_row) { return kMenuListY + kMenuRowPitch * float(p_row) + 17.0f; };
		CHECK(b.s.hud->hit_target(x, y_of(1)) == rows[1]);
		b.s.hud->mouse_move(x, y_of(1));
		CHECK(b.frame_label(rows[1]) == "over");
		CHECK(b.frame_label(rows[1] + ".bg") == "over");
		CHECK(b.s.hud->clip(rows[1] + ".Label.txtField").text_color() == 0xFFFFFFu);
		CHECK(b.frame_label(rows[0]) == "selected");
		b.events.clear();
		for (const size_t disabled : { size_t(2), size_t(4) }) {
			b.s.hud->mouse_move(x, y_of(disabled));
			CHECK(b.frame_label(rows[disabled]) == "inactive");
			b.s.hud->mouse_button(x, y_of(disabled), 1, true);
			b.s.hud->mouse_button(x, y_of(disabled), 1, false);
		}
		CHECK(b.released().empty());
		b.s.hud->mouse_move(x, y_of(5));
		b.s.hud->mouse_button(x, y_of(5), 1, true);
		b.s.hud->mouse_button(x, y_of(5), 1, false);
		const std::vector<HudWidgetEvent> out = b.released();
		REQUIRE(out.size() == 1);
		CHECK(out[0].param.to_string() == "ID_M_IGMMP_QUIT");
		CHECK(b.frame_label(rows[5]) == "overSelected");
	}

	TEST_CASE("menu: the page and mission headers take their static SIDs and the mode, level and team") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		MenuRig b;
		open_menu(*r, b, menu_model(true));
		CHECK(b.clip("PageHeader_01.heading").text() == "MULTIPLAYER /");
		CHECK(b.clip("PageHeader_01.subHeading").text() == "GAME MENU");
		CHECK(b.clip("MissionHeaderMP_01.text1.txt").text() == "TEAM DEATHMATCH");
		CHECK(b.clip("MissionHeaderMP_01.text2.txt").text() == "OPERATION M\xC3\x89TRO");
		CHECK(b.clip("MissionHeaderMP_02.text1.txt").text() == "YOUR TEAM");
		CHECK(b.clip("MissionHeaderMP_02.text2.txt").text() == "US");
		CHECK_FALSE(b.clip("MissionHeaderMP_01.icon").visible());
		const HudWidgetState *gm = b.s.hud->widget(std::string(kHudScreenInstance) + ".GamemodeInfo_01");
		REQUIRE(gm != nullptr);
		CHECK(gm->class_name == "Widget.Info.GamemodeInfo");
		CHECK(b.clip("GamemodeInfo_01").visible());
		for (const std::string_view panel : { "conquest", "rushAttacker", "rushDefender", "ctf" }) {
			CHECK_FALSE(b.clip("GamemodeInfo_01." + std::string(panel)).visible());
		}
		const HudWidgetState *om = b.s.hud->widget(std::string(kHudScreenInstance) + ".OverviewMap_01");
		REQUIRE(om != nullptr);
		CHECK(om->class_name == "Widget.Image.OverviewMap");
		bool renderer = false;
		for (const HudRendererString &rs : b.s.hud->renderer_strings()) {
			renderer = renderer ||
					(rs.path == std::string(kHudScreenInstance) + ".OverviewMap_01" && rs.renderer == kMinimapRenderer);
		}
		CHECK(renderer);
		CHECK(b.clip("OverviewMap_01").visible());
		CHECK(b.s.hud->deliver_in_event("OverviewMap_01", "UIWidgetEventID_Hide"));
		CHECK_FALSE(b.clip("OverviewMap_01").visible());
		std::printf("ui.menu headers '%s' '%s' / '%s' '%s'\n", b.clip("MissionHeaderMP_01.text1.txt").text().c_str(),
				b.clip("MissionHeaderMP_01.text2.txt").text().c_str(),
				b.clip("MissionHeaderMP_02.text1.txt").text().c_str(),
				b.clip("MissionHeaderMP_02.text2.txt").text().c_str());
	}

	TEST_CASE("menu: a dead soldier's SUICIDE row is inactive") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		MenuRig b;
		open_menu(*r, b, menu_model(false));
		const std::vector<std::string> rows = b.rows();
		REQUIRE(rows.size() == kHudMenuActionSids.size());
		CHECK(b.frame_label(rows[3]) == "inactive");
		HudModel alive = menu_model(true);
		b.s.hud->refresh(alive);
		const std::vector<std::string> again = b.rows();
		REQUIRE(again.size() == kHudMenuActionSids.size());
		CHECK(b.frame_label(again[3]) == "out");
	}

	TEST_CASE("menu: install unchanged") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const SnapshotDiff diff = diff_snapshots(r->mount->before, r->mount->reader.snapshot());
		std::printf("ui.menu install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
