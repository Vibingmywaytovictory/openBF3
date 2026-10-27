#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/ui_direct_access.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using ui_retail::Retail;
using ui_retail::retail_or_skip;

namespace {

constexpr std::string_view kScoreboard32 = "ui/flow/screen/scoreboards/scoreboardtwoteamshud32screen";
constexpr double kRowSpacing = 28.0;
constexpr std::string_view kPreRoundCategory = "ui/uicomponents/uipreroundcomp";
constexpr int32_t kPreRoundTitleKey = -566234449;
constexpr int32_t kPreRoundDescriptionKey = -1675254140;
constexpr std::string_view kAlert = "instance1.HudAlertMessage_03";
constexpr std::string_view kShowQuery = "UIWidgetEventID_Show";

struct Board {
	ui_retail::RetailScreen s;

	std::string path(std::string_view p_path) const {
		return std::string(kHudScreenInstance) + "." + std::string(p_path);
	}
	std::string row(std::string_view p_list, int32_t p_index) const {
		return s.row_at(path(std::string(p_list) + ".ListHolder"), kRowSpacing * p_index);
	}
	std::string text(const std::string &p_row, std::string_view p_field) const {
		return s.hud->clip(p_row + "." + std::string(p_field)).text();
	}
};

void add_player(HudModel &r_model, HudPlayerList &r_list, uint8_t p_slot, std::string_view p_name, bool p_bot) {
	HudPlayer &p = r_model.players[p_slot];
	std::memcpy(p.name.data(), p_name.data(), std::min(p_name.size(), p.name.size() - 1));
	p.bot = p_bot;
	r_model.player_count = uint8_t(std::max<int32_t>(r_model.player_count, p_slot + 1));
	r_list.index[r_list.count++] = p_slot;
}

HudModel footage_model() {
	HudModel m;
	m.local_player = 2;
	add_player(m, m.team.friendly_team_players, 0, "BOT_Sgt. Foster", true);
	add_player(m, m.team.friendly_team_players, 1, "BOT_Cpl. Nguyen", true);
	add_player(m, m.team.friendly_team_players, 2, "Maggy", false);
	add_player(m, m.team.enemy_team1_players, 3, "BOT_Maj. Belikov", true);
	m.players[0].kills = 2;
	m.players[0].deaths = 3;
	m.players[0].score = 572;
	m.players[1].kills = 4;
	m.players[1].deaths = 2;
	m.players[1].score = 430;
	m.players[2].ping_ms = 31;
	m.players[3].kills = 6;
	m.players[3].deaths = 2;
	m.players[3].score = 717;
	return m;
}

}

TEST_SUITE("ui-scoreboard") {
	TEST_CASE("scoreboard: the team lists' rows come from the model, bots without ping, voip or rank icons") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		Board b;
		b.s = ui_retail::open_retail_screen(*r, kScoreboard32);
		HudModel m = footage_model();
		b.s.hud->initialize(m);
		b.s.hud->refresh(m);
		CHECK(b.s.hud->stats().as2.missing_paths == 0);
		CHECK_MESSAGE(b.s.hud->stats().as2.failed_ops == 0, b.s.hud->stats().as2.last_failure);
		CHECK(b.s.hud->direct_access().errors().empty());
		const std::string foster = b.row("List_01", 0);
		const std::string nguyen = b.row("List_01", 1);
		const std::string maggy = b.row("List_01", 2);
		const std::string belikov = b.row("List_02", 0);
		REQUIRE_FALSE(foster.empty());
		REQUIRE_FALSE(nguyen.empty());
		REQUIRE_FALSE(maggy.empty());
		REQUIRE_FALSE(belikov.empty());
		CHECK(b.row("List_01", 3).empty());
		CHECK(b.row("List_02", 1).empty());
		CHECK(b.text(foster, "NumLabel") == "1");
		CHECK(b.text(foster, "Label") == "BOT_Sgt. Foster");
		CHECK(b.text(foster, "KillsLabel") == "2");
		CHECK(b.text(foster, "DeathsLabel") == "3");
		CHECK(b.text(foster, "ScoreLabel") == "572");
		CHECK(b.text(foster, "PingLabel") == "-");
		CHECK(b.text(nguyen, "Label") == "BOT_Cpl. Nguyen");
		CHECK(b.text(maggy, "Label") == "Maggy");
		CHECK(b.text(maggy, "PingLabel") == "31");
		CHECK(b.text(belikov, "Label") == "BOT_Maj. Belikov");
		CHECK(b.text(belikov, "ScoreLabel") == "717");
		CHECK(b.text(belikov, "PingLabel") == "-");
		for (const std::string &row : { foster, nguyen, maggy, belikov }) {
			CHECK_MESSAGE(b.s.drawn(row + ".voipIcon") == 0u, row);
			CHECK_MESSAGE(b.s.drawn(row + ".rankIcon") == 0u, row);
		}
		CHECK(b.s.hud->clip(foster + ".Label").text_color() == 0xFFFFFFu);
		CHECK(b.s.hud->clip(maggy + ".Label").text_color() == 0u);

		m.players[0].kills = 3;
		m.players[0].score = 672;
		b.s.hud->refresh(m);
		CHECK(b.text(foster, "KillsLabel") == "3");
		CHECK(b.text(foster, "ScoreLabel") == "672");
		CHECK(b.text(foster, "PingLabel") == "-");
	}

	TEST_CASE("scoreboard: preroundwaitingscreen's alert shows the flow's pre-round texts and CountLeft") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const TextDatabase &db = ui_retail::retail_text_db(*r);
		std::map<int32_t, std::string> flow;
		HudClientState client;
		client.flow_data = [&flow](std::string_view p_category, int32_t p_key) -> std::optional<std::string> {
			const auto it = flow.find(p_key);
			return p_category == kPreRoundCategory && it != flow.end() ? std::optional(it->second) : std::nullopt;
		};
		flow[kPreRoundTitleKey] = "ID_H_PREROUND_WAITING_FOR_PLAYERS_TITLE";
		flow[kPreRoundDescriptionKey] = "ID_H_PREROUND_WAITING_FOR_PLAYERS";
		ui_retail::RetailScreen s = ui_retail::open_retail_screen(
				*r, kPreRoundWaitingScreenPartition, ui_retail::retail_text_client(db, std::move(client)));
		std::vector<HudWidgetEvent> events;
		s.hud->set_event_sink([&events](const HudWidgetEvent &p_e) { events.push_back(p_e); });
		HudModel m;
		m.pre_round.count_left = 3;
		s.hud->initialize(m);
		s.hud->set_focus(true);
		const std::string alert = std::string(kAlert) + ".alert";
		const auto text = [&](std::string_view p_field) {
			return s.at(alert + ".content." + std::string(p_field)).text;
		};
		CHECK_FALSE(s.at(alert).visible);
		REQUIRE(s.hud->deliver_in_event("HudAlertMessage_03", kShowQuery, As2Value::of(0)));
		s.hud->advance(1.0 / 30.0);
		CHECK(s.at(kAlert).visible);
		CHECK(s.at(alert).visible);
		CHECK(text("title") == "WAITING FOR PLAYERS");
		CHECK(text("description") == "PLAYERS NEEDED");
		CHECK(text("timer") == "3");
		const auto fired = [&](std::string_view p_query) {
			return std::count_if(
					events.begin(), events.end(), [&](const HudWidgetEvent &e) { return e.query == p_query; });
		};
		CHECK(fired("UIWidgetEventID_OnItemPressed") == 1);
		CHECK(s.hud->stats().as2.missing_paths == 0);
		CHECK_MESSAGE(s.hud->stats().as2.failed_ops == 0, s.hud->stats().as2.last_failure);

		m.pre_round.count_left = 1;
		s.hud->refresh(m);
		CHECK(text("timer") == "1");

		flow[kPreRoundTitleKey] = "ID_H_PREROUND_COUNTDOWN_TITLE";
		flow[kPreRoundDescriptionKey] = "ID_H_PREROUND_COUNTDOWN";
		m.pre_round.count_left = 10;
		s.hud->refresh(m);
		CHECK(text("title") == "GET READY");
		CHECK(text("description") == "ROUND WILL START IN");
		CHECK(text("timer") == "10");
		CHECK(s.at(kAlert).visible);

		m.pre_round.count_left = 0;
		s.hud->refresh(m);
		CHECK_FALSE(s.at(kAlert).visible);

		flow[kPreRoundTitleKey] = "ID_H_PREROUND_RESTARTING_TITLE";
		flow[kPreRoundDescriptionKey] = "ID_H_PREROUND_RESTARTING";
		m.pre_round.count_left = 2;
		s.hud->refresh(m);
		CHECK(s.at(kAlert).visible);
		CHECK(text("title") == "TOO FEW PLAYERS");
		CHECK(text("description") == "RESTARTING ROUND");
	}

	TEST_CASE("scoreboard: MatchPreroundInformation writes its PreroundStatus and shows on ShowInformation") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const TextDatabase &db = ui_retail::retail_text_db(*r);
		ui_retail::RetailScreen s =
				ui_retail::open_retail_screen(*r, kPreRoundWaitingScreenPartition, ui_retail::retail_text_client(db));
		s.hud->initialize(HudModel{});
		const std::string info = "instance1.MatchPreroundInformation_01";
		CHECK_FALSE(s.at(info).visible);
		As2Value status = As2Value::object();
		status.set("PlayersReady", As2Value::of(3.7));
		status.set("PlayersRequired", As2Value::of(8));
		status.set("CountdownSeconds", As2Value::of(12));
		status.set("IsSpectator", As2Value::of(false));
		status.set("IsLocalPlayerReady", As2Value::of(true));
		const HudWidgetState *w = s.hud->widget(info);
		REQUIRE(w != nullptr);
		w->controller->update(*w->host, "PreroundStatus", status);
		CHECK(s.at(info + ".playersJoined").text == "3/8 PLAYERS READY");
		CHECK(s.at(info + ".timer").text == "12");
		CHECK(s.at(info + ".playerStatus").text ==
				std::string(db.find_sid("ID_M_PREROUND_HUD_READYSTATE").value_or("?")));
		status.set("IsLocalPlayerReady", As2Value::of(false));
		w->controller->update(*w->host, "PreroundStatus", status);
		CHECK(s.at(info + ".playerStatus").text ==
				std::string(db.find_sid("ID_M_PREROUND_HUD_NOTREADYSTATE").value_or("?")));
		status.set("IsSpectator", As2Value::of(true));
		w->controller->update(*w->host, "PreroundStatus", status);
		CHECK(s.at(info + ".playerStatus").text ==
				std::string(db.find_sid("ID_M_PREROUND_HUD_SPECTATOR").value_or("?")));
		REQUIRE(s.hud->deliver_in_event("MatchPreroundInformation_01", kShowQuery, As2Value::of(0)));
		CHECK(s.at(info).visible);
		REQUIRE(s.hud->deliver_in_event("MatchPreroundInformation_01", "UIWidgetEventID_Hide", As2Value::of(0)));
		CHECK_FALSE(s.at(info).visible);
		CHECK(s.hud->stats().as2.missing_paths == 0);
		CHECK_MESSAGE(s.hud->stats().as2.failed_ops == 0, s.hud->stats().as2.last_failure);
	}
}
