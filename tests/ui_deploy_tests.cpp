#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_hit.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/text_db.h"
#include "button.h"
#include "deploy_widgets.h"
#include "list.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui_retail;

namespace {

constexpr std::string_view kSpawnButtonScreen = "ui/flow/screen/spawnbuttonscreen";
constexpr std::string_view kSpawnTimer = "instance1.SpawnTimer_01";
constexpr std::string_view kSpawnScreenPc = "ui/flow/screen/spawnscreenpc";
constexpr std::string_view kTicketScreen = "ui/flow/screen/spawnscreenticketcountertdmscreen";
constexpr std::string_view kTicket = "instance1.TicketCounterSpawnScreen_01.Ticket";
constexpr uint32_t kUsHash = 0x5501u;
constexpr uint32_t kRuHash = 0x5502u;

std::pair<float, float> stage_centre(const RetailScreen &p_s, std::string_view p_path) {
	const SwfSceneNode &n = p_s.at(p_path);
	const SwfRect b = swf_node_local_bounds(*p_s.scene, n);
	const SwfMatrix w = swf_world_matrix(*p_s.scene, n.id);
	return { w.tx + w.a * (b.x_min + b.x_max) * 0.5f, w.ty + w.d * (b.y_min + b.y_max) * 0.5f };
}

HudModel dead_for(float p_seconds, bool p_can_spawn) {
	HudModel m{};
	m.spawn_logic.time_to_respawn = p_seconds;
	m.spawn_points.can_spawn = p_can_spawn;
	return m;
}

std::string clip(std::string_view p_leaf) {
	return std::string(kSpawnTimer) + "." + std::string(p_leaf);
}

constexpr std::string_view kCustomizeSub = "ui/flow/screen/customizesoldiersubscreen";
constexpr std::string_view kCustomizeSoldier = "ui/flow/screen/customizesoldierscreen";
constexpr std::string_view kCustomizeAccessories = "ui/flow/screen/customizeaccessoriesscreen";

uint32_t item_name_hash(size_t p_list, size_t p_item) {
	return 0x7000u + uint32_t(p_list) * 0x10u + uint32_t(p_item);
}

HudClientState customize_client(Retail &r_r) {
	HudClientState c = retail_text_client(retail_text_db(r_r));
	static const std::optional<ItemIconIndex> items = [&]() -> std::optional<ItemIconIndex> {
		Result<ItemIconIndex> read = ItemIconIndex::read(*r_r.ui);
		REQUIRE_MESSAGE(read.ok(), (read.ok() ? std::string() : read.error().message));
		return std::move(*read);
	}();
	c.items = &*items;
	c.localise_hash = [base = c.localise_hash](uint32_t p_hash) {
		if (p_hash == 0x6000u) {
			return std::string("ASSAULT");
		}
		if (p_hash >= 0x6100u && p_hash < 0x6110u) {
			return "CATEGORY " + std::to_string(p_hash - 0x6100u);
		}
		if (p_hash >= 0x7000u && p_hash < 0x8000u) {
			return "ITEM " + std::to_string((p_hash - 0x7000u) / 0x10u) + "." + std::to_string(p_hash % 0x10u);
		}
		return base ? base(p_hash) : std::string();
	};
	return c;
}

HudModel customize_model() {
	HudModel m{};
	m.kit.selected_kit_name = 0x6000u;
	HudCustomization &c = m.customization;
	for (size_t l = 0; l < kHudSpawnKitItems; ++l) {
		c.kit_grid_data[l].count = 3;
		for (size_t i = 0; i < 3; ++i) {
			c.kit_grid_data[l].items[i].item_id = uint32_t(100 * (l + 1) + i);
			c.kit_grid_data[l].items[i].name_sid_hash = item_name_hash(l, i);
		}
		c.kit_grid_selected[l] = 1;
		c.kit_grid_category[l] = 0x6100u + uint32_t(l);
	}
	c.kit_grid_data[0].items[1].item_id = 2580429694u;
	for (size_t a = 0; a < kHudAccessorySlots; ++a) {
		c.accessory_grid[a].count = 2;
		for (size_t i = 0; i < 2; ++i) {
			c.accessory_grid[a].items[i].item_id = uint32_t(900 + 10 * a + i);
			c.accessory_grid[a].items[i].name_sid_hash = item_name_hash(5 + a, i);
		}
		c.accessory_selected[a] = 0;
	}
	for (size_t k = 0; k < kHudKitCount; ++k) {
		c.kits_data[k].name_sid_hash = k == 0 ? 0x6000u : 0u;
		for (size_t i = 0; i < kHudSpawnKitItems; ++i) {
			c.kits_data[k].items[i].name_sid_hash = item_name_hash(i, 1);
		}
	}
	return m;
}

std::string slot_path(std::string_view p_widget, int p_slot) {
	return "instance1." + std::string(p_widget) + ".mcContainer.Slot_" + std::to_string(p_slot);
}

}

TEST_SUITE("ui-deploy") {
	TEST_CASE("customizesoldierscreen KitView_01: four kit slots 111 px apart as 18.57 draws them (the slot's shown "
			  "bounds plus SLOT_PADDING), with the kit's name, the five items' names and the LOADOUT and APPEARANCE "
			  "buttons; customizeaccessoriesscreen: the primary and its four accessory "
			  "selectors") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		{
			RetailScreen s = open_retail_screen(*r, kCustomizeSoldier, customize_client(*r));
			CHECK(s.hud->stats().unknown_class_paths.empty());
			std::vector<std::string> fired;
			s.hud->set_event_sink([&](const HudWidgetEvent &e) {
				fired.push_back(e.instance + " " + e.query + " " + e.param.to_string());
			});
			s.hud->initialize(customize_model());
			CHECK(s.hud->deliver_in_event("KitView_01", "UIWidgetEventID_OnItemHighlighted", As2Value::of("2")));
			s.hud->advance(1.0 / 30.0);
			CHECK(std::find(fired.begin(), fired.end(), "KitView_01 UIWidgetEventID_OnItemSelected 2") != fired.end());
			CHECK(std::find(fired.begin(), fired.end(), "KitView_01 UIWidgetEventID_OnItemSelected 0") == fired.end());
			for (int k = 0; k < 4; ++k) {
				CHECK(s.at(slot_path("KitView_01", k)).visible);
			}
			const std::string first = slot_path("KitView_01", 0);
			CHECK(std::fabs(s.at(slot_path("KitView_01", 1)).matrix.ty - 111.0f) <= 1.0f);
			CHECK(s.hud->clip(first + ".header.txt").text() == "ASSAULT");
			CHECK(s.hud->clip(first + ".slotLabel1.txt").text() == "ITEM 0.1");
			CHECK(s.hud->clip(first + ".slotLabel5.txt").text() == "ITEM 4.1");
			CHECK(s.at(first + ".headerIcon").visible);
			CHECK(s.at(first + ".button1").visible);
			CHECK(s.at(first + ".button2").visible);
			CHECK(s.hud->clip(first + ".button1.button.label.txt").text() == "LOADOUT");
			CHECK(s.hud->clip(first + ".button2.button.label.txt").text() == "APPEARANCE");
			const auto [x, y] = stage_centre(s, first + ".button1");
			fired.clear();
			s.hud->mouse_move(x, y);
			s.hud->mouse_button(x, y, 1, true);
			s.hud->mouse_button(x, y, 1, false);
			CHECK(std::find(fired.begin(), fired.end(), "KitView_01 UIWidgetEventID_OnItemReleased 0") != fired.end());
			CHECK(s.hud->stats().as2.failed_ops == 0);
		}
		RetailScreen s = open_retail_screen(*r, kCustomizeAccessories, customize_client(*r));
		CHECK(s.hud->stats().unknown_class_paths.empty());
		s.hud->initialize(customize_model());
		s.hud->advance(1.0 / 30.0);
		CHECK(s.hud->clip(slot_path("KitSelector_01", 0) + ".header.txt").text() == "ITEM 0.1");
		for (int a = 0; a < 4; ++a) {
			CHECK(s.hud->clip(slot_path("KitSelector_0" + std::to_string(a + 2), 0) + ".header.txt").text() ==
					"ITEM " + std::to_string(5 + a) + ".0");
		}
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
	TEST_CASE(
			"customizesoldiersubscreen: the five KitSelectors draw the kit's items, the selected one in the header "
			"with its category, the kit name in TextField_01, and an arrow click steps the item and sends its index") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kCustomizeSub, customize_client(*r));
		CHECK(s.hud->stats().unknown_class_paths.empty());
		std::vector<std::string> fired;
		s.hud->set_event_sink([&](const HudWidgetEvent &e) {
			fired.push_back(e.instance + " " + e.query + " " + e.param.to_string());
		});
		s.hud->initialize(customize_model());
		s.hud->advance(1.0 / 30.0);
		for (int w = 1; w <= 5; ++w) {
			const std::string widget = "KitSelector_0" + std::to_string(w);
			const std::string slot = slot_path(widget, 0);
			CHECK(s.at("instance1." + widget).visible);
			CHECK(s.at(slot).visible);
			CHECK(s.hud->clip(slot + ".header.txt").text() == "ITEM " + std::to_string(w - 1) + ".1");
			CHECK(s.hud->clip(slot + ".category").text() == "CATEGORY " + std::to_string(w - 1));
		}
		CHECK(s.hud->clip(slot_path("KitSelector_01", 0) + ".headerIcon.txt").text() == "0");
		CHECK(s.hud->clip(slot_path("KitSelector_01", 0) + ".button1.button.label.txt").text() == "ACCESSORIES");
		CHECK(s.hud->clip("instance1.TextField_01").exists());
		std::vector<HudWidgetEvent> routed;
		s.hud->set_event_sink([&](const HudWidgetEvent &e) { routed.push_back(e); });
		const auto [ix, iy] = stage_centre(s, slot_path("KitSelector_01", 0) + ".mouseArea");
		s.hud->mouse_move(ix, iy);
		for (const HudWidgetEvent &e : routed) {
			if (e.instance == "KitSelector_01" && e.query == "UIWidgetEventID_OnItemOver") {
				CHECK(s.hud->deliver_in_event("KitInfoBox_01", "UIWidgetEventID_OnShow"));
			} else if (e.instance == "KitSelector_01" && e.query == "UIWidgetEventID_Update") {
				CHECK(s.hud->deliver_in_event("KitInfoBox_01", "UIWidgetEventID_Update", e.param));
			}
		}
		s.hud->advance(1.0 / 30.0);
		const std::string box = "instance1.KitInfoBox_01";
		CHECK(s.at(box).visible);
		CHECK(s.hud->clip(box + ".ammoValue").text() == "30 [5.56x45mm NATO]");
		CHECK(s.hud->clip(box + ".rofValue").text() == "800");
		CHECK(s.hud->clip(box + ".rangeValue").text() == "LONG");
		CHECK(s.hud->clip(box + ".fireMode1").current_frame() == 1);
		CHECK(s.hud->clip(box + ".fireMode0").current_frame() == 3);
		CHECK_FALSE(s.at(box + ".fireMode2").visible);
		s.hud->set_event_sink([&](const HudWidgetEvent &e) {
			fired.push_back(e.instance + " " + e.query + " " + e.param.to_string());
		});
		fired.clear();
		const auto [x, y] = stage_centre(s, slot_path("KitSelector_02", 0) + ".buttonRight");
		s.hud->mouse_move(x, y);
		s.hud->mouse_button(x, y, 1, true);
		s.hud->mouse_button(x, y, 1, false);
		CHECK(s.hud->clip(slot_path("KitSelector_02", 0) + ".header.txt").text() == "ITEM 1.2");
		CHECK(std::find(fired.begin(), fired.end(), "KitSelector_02 UIWidgetEventID_SetIndex 2") != fired.end());
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
	TEST_CASE("SpawnTimer: the countdown dims the widget to 20 and shows TIME TO DEPLOY with the light bars at "
			  "Time + 2; a click while counting arms the deploy, which fires the Deploy port when Time reaches 0") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kSpawnButtonScreen, retail_text_client(retail_text_db(*r)));
		CHECK(s.hud->stats().unknown_class_paths.empty());
		auto *timer = dynamic_cast<HudSpawnTimer *>(s.hud->controller(kSpawnTimer));
		REQUIRE(timer != nullptr);
		std::vector<std::string> fired;
		s.hud->set_event_sink([&](const HudWidgetEvent &e) { fired.push_back(e.query + " " + e.param.to_string()); });
		CHECK(hud_spawn_timer_seconds(4.2f) == 5);
		CHECK(hud_spawn_timer_seconds(0.0f) == 0);
		CHECK(hud_spawn_timer_seconds(-1.0f) == 0);
		s.hud->initialize(dead_for(4.2f, true));
		CHECK(timer->current_time() == doctest::Approx(5.0));
		CHECK(timer->start_time() == doctest::Approx(5.0));
		CHECK(s.at(kSpawnTimer).visible);
		CHECK(s.hud->clip(kSpawnTimer).alpha() == doctest::Approx(20.0f));
		CHECK(s.at(clip("countdown")).visible);
		CHECK(s.at(clip("countdown.textCountdown")).text == "5");
		CHECK(s.at(clip("deployStatus")).visible);
		CHECK(s.at(clip("deployStatus.textNormal.txt")).text == "TIME TO DEPLOY");
		CHECK_FALSE(s.at(clip("deploy")).visible);
		CHECK(s.hud->clip(clip("lightbars")).current_frame() == 7);
		CHECK(s.at(clip("deploy.textDeploy.txt")).text == "DEPLOY");
		s.hud->advance(0.5);
		CHECK(timer->current_frame() > 0);
		const auto [cx, cy] = stage_centre(s, clip("btnDeploy"));
		CHECK(s.hud->hit_target(cx, cy) == clip("btnDeploy"));
		s.hud->mouse_move(cx, cy);
		s.hud->mouse_button(cx, cy, 1, true);
		s.hud->mouse_button(cx, cy, 1, false);
		CHECK(timer->deployable());
		CHECK(fired.empty());
		s.hud->refresh(dead_for(2.0f, true));
		CHECK(s.at(clip("countdown.textCountdown")).text == "2");
		CHECK(s.hud->clip(clip("lightbars")).current_frame() == 4);
		CHECK(fired.empty());
		s.hud->refresh(dead_for(0.0f, true));
		CHECK(timer->current_time() == doctest::Approx(0.0));
		CHECK(s.hud->clip(kSpawnTimer).alpha() == doctest::Approx(100.0f));
		CHECK(s.at(clip("deploy")).visible);
		CHECK_FALSE(s.at(clip("deployStatus")).visible);
		CHECK_FALSE(s.at(clip("lightbars")).visible);
		CHECK(s.at(clip("countdown.textCountdown")).text.empty());
		REQUIRE(fired.size() == 1);
		CHECK(fired[0] == "UIWidgetEventID_OnItemReleased true");
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}

	TEST_CASE("SpawnTimer: at Time 0 a btnDeploy release fires Deploy; CanSpawn false disarms and refuses it and "
			  "dims the widget") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kSpawnButtonScreen, retail_text_client(retail_text_db(*r)));
		auto *timer = dynamic_cast<HudSpawnTimer *>(s.hud->controller(kSpawnTimer));
		REQUIRE(timer != nullptr);
		std::vector<std::string> fired;
		s.hud->set_event_sink([&](const HudWidgetEvent &e) { fired.push_back(e.query + " " + e.param.to_string()); });
		s.hud->initialize(dead_for(0.0f, true));
		CHECK(fired.empty());
		CHECK(s.at(clip("deploy")).visible);
		CHECK(s.hud->clip(kSpawnTimer).alpha() == doctest::Approx(100.0f));
		s.hud->refresh(dead_for(0.0f, false));
		CHECK_FALSE(timer->can_spawn());
		CHECK(s.hud->clip(kSpawnTimer).alpha() == doctest::Approx(20.0f));
		const auto [cx, cy] = stage_centre(s, clip("btnDeploy"));
		s.hud->mouse_move(cx, cy);
		s.hud->mouse_button(cx, cy, 1, true);
		s.hud->mouse_button(cx, cy, 1, false);
		CHECK(fired.empty());
		s.hud->refresh(dead_for(0.0f, true));
		CHECK(fired.empty());
		s.hud->mouse_button(cx, cy, 1, true);
		s.hud->mouse_button(cx, cy, 1, false);
		REQUIRE(fired.size() == 1);
		CHECK(fired[0] == "UIWidgetEventID_OnItemReleased false");
		CHECK(timer->deploys_fired() == 1);
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
	TEST_CASE("TicketCounterSpawnScreen on spawnscreenticketcountertdmscreen: m_gamemode tdm, the friendly name and "
			  "tickets on the left of the 769 x 97 bar, the enemy's on the right, as footage 18.57 shows US 5 | 8 RU") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudClientState c = retail_text_client(retail_text_db(*r));
		c.localise_hash = [](uint32_t p_hash) {
			return p_hash == kUsHash ? std::string("US") : p_hash == kRuHash ? std::string("RU") : std::string();
		};
		RetailScreen s = open_retail_screen(*r, kTicketScreen, c);
		CHECK(s.hud->stats().unknown_class_paths.empty());
		CHECK_FALSE(s.at("instance1.TicketCounterSpawnScreen_01").visible);
		HudModel m{};
		m.kill_counter.player_team = 1;
		m.kill_counter.player_team_kills = 5;
		m.kill_counter.enemy_team_kills = 8;
		m.team.friendly_team_name = kUsHash;
		m.team.enemy_team1_name = kRuHash;
		s.hud->initialize(m);
		CHECK(s.at("instance1.TicketCounterSpawnScreen_01").visible);
		const std::string t(kTicket);
		CHECK(s.at(t + ".txtTeam1Team").text == "US");
		CHECK(s.at(t + ".txtTeam1Tickets").text == "5");
		CHECK(s.at(t + ".txtTeam2Tickets").text == "8");
		CHECK(s.at(t + ".txtTeam2Team").text == "RU");
		const SwfMatrix w = swf_world_matrix(*s.scene, s.at(t).id);
		CHECK(w.tx == doctest::Approx(769.0f));
		CHECK(w.ty == doctest::Approx(97.0f));
		m.kill_counter.enemy_team_kills = 9;
		s.hud->refresh(m);
		CHECK(s.at(t + ".txtTeam2Tickets").text == "9");
		CHECK(s.at(t + ".txtTeam1Tickets").text == "5");
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
	TEST_CASE("spawnscreenpc header and overview: SpawnPointsHeader reads US then DEPLOY POINTS 10 px after it, as "
			  "footage 18.57 shows; OverviewMap_01 draws through the minimap renderer and Hide/Show toggle it") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudClientState c = retail_text_client(retail_text_db(*r));
		c.localise_hash = [](uint32_t p_hash) { return p_hash == kUsHash ? std::string("US") : std::string(); };
		RetailScreen s = open_retail_screen(*r, kSpawnScreenPc, c);
		HudModel m{};
		m.team.friendly_team_name = kUsHash;
		m.team.friendly_team = 1;
		s.hud->initialize(m);
		const As2Clip t1 = s.hud->clip("instance1.SpawnPointsHeader_01.txt1");
		const As2Clip t2 = s.hud->clip("instance1.SpawnPointsHeader_01.txt2");
		CHECK(t1.text() == "US");
		CHECK(t2.text() == "DEPLOY POINTS");
		CHECK(t2.x() == doctest::Approx(t1.x() + t1.text_width() + 10.0f));
		CHECK(s.hud->clip("instance1.SpawnPointsHeader_01").xscale() == doctest::Approx(100.0f));
		const std::string map = "instance1.OverviewMap_01";
		CHECK(s.at(map).visible);
		const bool rendered = std::any_of(s.hud->renderer_strings().begin(), s.hud->renderer_strings().end(),
				[&](const HudRendererString &p_r) { return p_r.path == map && p_r.renderer == kMinimapRenderer; });
		CHECK(rendered);
		CHECK(s.hud->deliver_in_event("OverviewMap_01", "UIWidgetEventID_Hide"));
		CHECK_FALSE(s.at(map).visible);
		CHECK(s.hud->deliver_in_event("OverviewMap_01", "UIWidgetEventID_Show"));
		CHECK(s.at(map).visible);
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
	TEST_CASE("spawnscreenpc List_01: the TDM random spawner's row is a baseSpawn row reading RANDOM SPAWN POINT, "
			  "drawn selected beside the blue icon box with no letter and the blank vehicle icon, as footage 18.57 "
			  "shows it") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kSpawnScreenPc, retail_text_client(retail_text_db(*r)));
		HudModel m{};
		HudSpawnPointList &list = m.spawn_points.spawn_points;
		list.count = 1;
		list.points[0].kind = HudSpawnKind::Hq;
		list.points[0].team = 1;
		list.points[0].available = true;
		list.points[0].label_sid_hash = text_hash("ID_H_MP_DM_RANDOMSPAWN");
		s.hud->initialize(m);
		std::vector<std::string> rows;
		for (const std::unique_ptr<HudWidgetState> &w : s.hud->widgets()) {
			if (w->class_name == kSpawnRowClass && !w->retired) {
				rows.push_back(w->path);
			}
		}
		REQUIRE(rows.size() == 1);
		const As2Clip label = s.hud->clip(rows[0] + ".Label.txtField");
		CHECK(label.text() == "RANDOM SPAWN POINT");
		CHECK(s.hud->clip(rows[0] + ".IconLabel.txtField").text().empty());
		const SwfSceneNode &box = s.at(rows[0] + ".icon_bg");
		REQUIRE(box.timeline != nullptr);
		CHECK(box.visible);
		CHECK(box.timeline->label_frame("blue") == box.frame);
		CHECK(s.at(rows[0]).visible);
		CHECK(s.at("instance1.List_01").visible);
		auto *row = dynamic_cast<RowBaseClass *>(s.hud->controller(rows[0]));
		REQUIRE(row != nullptr);
		CHECK(row->highlighted());
		CHECK(row->state() == "selected");
		const SwfSceneNode &drawn = s.at(rows[0]);
		REQUIRE(drawn.timeline != nullptr);
		CHECK(drawn.timeline->label_frame("selected") == drawn.frame);
		const SwfSceneNode &bg = s.at(rows[0] + ".bg");
		REQUIRE(bg.timeline != nullptr);
		CHECK(bg.timeline->label_frame("selected") == bg.frame);
		CHECK(label.text_color() == 0x000000u);
		const SwfSceneNode &icon = s.at(rows[0] + ".vehicleIcon");
		REQUIRE(icon.timeline != nullptr);
		CHECK(icon.timeline->label_frame(kSpawnRowNoVehicleIcon) == icon.frame);
		CHECK(icon.children.empty());
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
	TEST_CASE("spawnscreenpc List_01: a row's highlight and release send its point id, as SendIndexWithEvent asks; the "
			  "selected point is the highlighted row, else the first available one") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m{};
		HudSpawnPointList &list = m.spawn_points.spawn_points;
		list.count = 3;
		for (size_t i = 0; i < 3; ++i) {
			list.points[i].kind = i == 0 ? HudSpawnKind::Hq : HudSpawnKind::Flag;
			list.points[i].team = 1;
			list.points[i].available = i != 2;
		}
		const HudClientState client = retail_text_client(retail_text_db(*r));
		HudDataSource list_query;
		list_query.category = std::string(kSpawnPointListCategory);
		list_query.key = kSpawnPointListKey;
		const std::optional<As2Value> rows = hud_model_list_rows(list_query, m, client);
		REQUIRE(rows.has_value());
		REQUIRE(rows->size() == 3);
		CHECK(rows->at(1).field("id").to_number() == doctest::Approx(1.0));
		HudDataSource selection;
		selection.category = std::string(kSpawnPointListCategory);
		selection.key = kDefaultSpawnPointKey;
		CHECK(hud_model_list_selection(selection, m) == 0);
		m.spawn_points.selected_point = 1;
		CHECK(hud_model_list_selection(selection, m) == 1);
		m.spawn_points.selected_point = 2;
		CHECK(hud_model_list_selection(selection, m) == 0);
		m.spawn_points.selected_point = 0;
		RetailScreen s = open_retail_screen(*r, kSpawnScreenPc, client);
		std::vector<std::string> fired;
		s.hud->set_event_sink([&](const HudWidgetEvent &e) {
			if (e.instance == "List_01" && !e.query.starts_with("UIWidgetEventID_PlaySound")) {
				fired.push_back(e.query + " " + e.param.to_string());
			}
		});
		s.hud->initialize(m);
		std::vector<std::string> base_rows;
		std::string point_1;
		for (const std::unique_ptr<HudWidgetState> &w : s.hud->widgets()) {
			if (w->class_name == kSpawnRowClass && !w->retired) {
				base_rows.push_back(w->path);
				const auto *row = dynamic_cast<const RowBaseClass *>(s.hud->controller(w->path));
				if (row != nullptr && row->row_index() == 1.0) {
					point_1 = w->path;
				}
			}
		}
		REQUIRE(base_rows.size() == 3);
		REQUIRE_FALSE(point_1.empty());
		CHECK(std::find(fired.begin(), fired.end(), "UIWidgetEventID_OnItemHighlighted 0") != fired.end());
		fired.clear();
		const auto [x, y] = stage_centre(s, point_1);
		s.hud->mouse_move(x, y);
		s.hud->mouse_button(x, y, 1, true);
		s.hud->mouse_button(x, y, 1, false);
		CHECK(std::find(fired.begin(), fired.end(), "UIWidgetEventID_OnItemReleased 1") != fired.end());
		CHECK(std::find(fired.begin(), fired.end(), "UIWidgetEventID_OnItemHighlighted 1") != fired.end());
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
	TEST_CASE("spawnscreenpc KitBar: DEPLOY AS and the kit name, four kit buttons with the selected one lit, five slot "
			  "items; a kitButton_2 click fires KitChanged 2 and relabels; a slot click opens the grid, "
			  "DoneSelectingSlotItem closes it") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudClientState c = retail_text_client(retail_text_db(*r));
		const std::array<std::string, 4> kit_names{ "ASSAULT", "ENGINEER", "SUPPORT", "RECON" };
		c.localise_hash = [&](uint32_t p_hash) {
			if (p_hash >= 0x100u && p_hash < 0x104u) {
				return kit_names[p_hash - 0x100u];
			}
			return p_hash >= 0x200u && p_hash < 0x300u ? "ITEM " + std::to_string(p_hash - 0x200u) : std::string();
		};
		RetailScreen s = open_retail_screen(*r, kSpawnScreenPc, c);
		std::vector<std::string> fired;
		s.hud->set_event_sink([&](const HudWidgetEvent &e) {
			if (e.instance == "KitBar_01" && !e.query.starts_with("UIWidgetEventID_PlaySound")) {
				fired.push_back(e.query + " " + e.param.to_string());
			}
		});
		HudModel m{};
		for (uint32_t k = 0; k < 4; ++k) {
			HudKitEntry &e = m.customization.kits_data[k];
			e.name_sid_hash = 0x100u + k;
			e.hud_icon = int32_t(k + 1);
			for (uint32_t i = 0; i < kHudSpawnKitItems; ++i) {
				e.items[i].name_sid_hash = 0x200u + k * 10u + i;
			}
		}
		m.kit.selected_kit = 0;
		s.hud->initialize(m);
		const std::string bar = "instance1.KitBar_01";
		auto *kit_bar = dynamic_cast<HudKitBar *>(s.hud->controller(bar));
		REQUIRE(kit_bar != nullptr);
		CHECK(s.at(bar).visible);
		CHECK(kit_bar->current_kit() == 0);
		const As2Clip deploy_as = s.hud->clip(bar + ".deployAs");
		const As2Clip kit_name = s.hud->clip(bar + ".kitName");
		CHECK(deploy_as.text() == "DEPLOY AS");
		CHECK(kit_name.text() == "ASSAULT");
		CHECK(kit_name.x() == doctest::Approx(deploy_as.x() + 10.0f + deploy_as.text_width()));
		CHECK(s.hud->clip(bar + ".kitItem_0.txt").text() == "ITEM 0");
		CHECK(s.hud->clip(bar + ".kitItem_4.txt").text() == "ITEM 4");
		CHECK_FALSE(s.at(bar + ".progressBar").visible);
		CHECK_FALSE(s.at(bar + ".changeItemText").visible);
		auto *b0 = dynamic_cast<HudButton *>(s.hud->controller(bar + ".kitButton_0"));
		auto *b2 = dynamic_cast<HudButton *>(s.hud->controller(bar + ".kitButton_2"));
		REQUIRE(b0 != nullptr);
		REQUIRE(b2 != nullptr);
		CHECK(b0->selected());
		REQUIRE(fired.size() >= 2);
		CHECK(fired[0] == "UIWidgetEventID_OnItemReleased 0");
		fired.clear();
		const auto [kx, ky] = stage_centre(s, bar + ".kitButton_2");
		s.hud->mouse_move(kx, ky);
		s.hud->mouse_button(kx, ky, 1, true);
		s.hud->mouse_button(kx, ky, 1, false);
		CHECK(kit_bar->current_kit() == 2);
		CHECK(b2->selected());
		CHECK_FALSE(b0->selected());
		CHECK(kit_name.text() == "SUPPORT");
		CHECK(s.hud->clip(bar + ".kitItem_0.txt").text() == "ITEM 20");
		REQUIRE_FALSE(fired.empty());
		CHECK(fired[0] == "UIWidgetEventID_OnItemReleased 2");
		fired.clear();
		const auto [sx, sy] = stage_centre(s, bar + ".slotButton_1");
		s.hud->mouse_move(sx, sy);
		s.hud->mouse_button(sx, sy, 1, true);
		s.hud->mouse_button(sx, sy, 1, false);
		CHECK(kit_bar->changing_item());
		CHECK(kit_bar->current_slot() == 1);
		const std::vector<std::string> open{ "UIWidgetEventID_SetIndex 1", "UIWidgetEventID_OnShow 0" };
		CHECK(fired == open);
		fired.clear();
		CHECK(s.hud->deliver_in_event("KitBar_01", "UIWidgetEventID_OnItemSelected"));
		CHECK_FALSE(kit_bar->changing_item());
		const std::vector<std::string> close{ "UIWidgetEventID_OnHide 0" };
		CHECK(fired == close);
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
	TEST_CASE("spawnscreenpc Grid_01: the SELECT ITEM picker draws one KitCell per item of the selected slot with the "
			  "selected one highlighted, slides up on Show, and a cell click fires Released with its index") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudClientState c = retail_text_client(retail_text_db(*r));
		c.localise_hash = [](uint32_t p_hash) {
			return p_hash >= 0x300u && p_hash < 0x320u ? "GUN " + std::to_string(p_hash - 0x300u) : std::string();
		};
		RetailScreen s = open_retail_screen(*r, kSpawnScreenPc, c);
		std::vector<std::string> fired;
		s.hud->set_event_sink([&](const HudWidgetEvent &e) {
			if (e.instance == "Grid_01" && !e.query.starts_with("UIWidgetEventID_PlaySound")) {
				fired.push_back(e.query + " " + e.param.to_string());
			}
		});
		HudModel m{};
		HudItemList &list = m.customization.kit_grid_data[0];
		list.count = 4;
		for (uint32_t i = 0; i < list.count; ++i) {
			list.items[i].item_id = 1000u + i;
			list.items[i].name_sid_hash = 0x300u + i;
		}
		m.customization.kit_grid_selected[0] = 1;
		m.customization.selected_slot = 0;
		s.hud->initialize(m);
		const std::string grid = "instance1.Grid_01";
		auto *g = dynamic_cast<HudGrid *>(s.hud->controller(grid));
		REQUIRE(g != nullptr);
		CHECK(g->cell_count() == 4);
		CHECK(g->selected_index() == 1);
		CHECK(g->columns() >= 1);
		const std::string cell1 = grid + ".gridItems." + g->cell_name(1);
		s.hud->advance(0.1);
		CHECK(s.hud->clip(grid + ".gridItems." + g->cell_name(0) + ".label.txt").text() == "GUN 0");
		CHECK(s.at(cell1 + ".highlight").visible);
		CHECK_FALSE(s.at(grid + ".gridItems." + g->cell_name(0) + ".highlight").visible);
		const float hidden_y = s.hud->clip(grid + ".gridItems").y();
		HudWidgetHost *host = s.hud->host(grid);
		REQUIRE(host != nullptr);
		s.hud->controller(grid)->in_event(*host, 10, As2Value::of(0));
		s.hud->advance(1.0);
		CHECK(s.at(grid).visible);
		CHECK(s.hud->clip(grid + ".gridItems").y() < hidden_y);
		CHECK(s.hud->clip(grid + ".gridItems").alpha() == doctest::Approx(75.0f));
		fired.clear();
		const As2Clip target = s.hud->clip(cell1 + ".bg").exists() ? s.hud->clip(cell1 + ".bg") : s.hud->clip(cell1);
		const auto [x, y] = stage_centre(s, target.path());
		CHECK(s.hud->hit_target(x, y) == target.path());
		s.hud->mouse_move(x, y);
		s.hud->mouse_button(x, y, 1, true);
		s.hud->mouse_button(x, y, 1, false);
		CHECK(std::find(fired.begin(), fired.end(), "UIWidgetEventID_OnItemReleased 1") != fired.end());
		s.hud->controller(grid)->in_event(*host, 11, As2Value::of(0));
		s.hud->advance(1.0);
		CHECK_FALSE(s.at(grid).visible);
		CHECK(s.hud->stats().as2.failed_ops == 0);
	}
	TEST_CASE("spawnscreenpc: every AS2 class on the screen has its controller") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kSpawnScreenPc, retail_text_client(retail_text_db(*r)));
		std::string unknown;
		for (const std::string &p : s.hud->stats().unknown_class_paths) {
			unknown += p + " ";
		}
		CHECK_MESSAGE(s.hud->stats().unknown_class_paths.empty(), unknown);
	}
}
