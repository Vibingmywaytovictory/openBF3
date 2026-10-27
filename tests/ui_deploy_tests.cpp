#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/swf_hit.h"
#include "bf3/core/ui/swf_scene.h"
#include "deploy_widgets.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <string>
#include <utility>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui_retail;

namespace {

constexpr std::string_view kSpawnButtonScreen = "ui/flow/screen/spawnbuttonscreen";
constexpr std::string_view kSpawnTimer = "instance1.SpawnTimer_01";
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

}

TEST_SUITE("ui-deploy") {
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
}
