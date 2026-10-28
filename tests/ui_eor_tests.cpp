#include <doctest/doctest.h>

#include "bf3/core/level/budget.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui_retail;

namespace {

constexpr std::string_view kEorDetailsScoreboard = "ui/flow/screen/eordetailsscoreboardscreen";
constexpr std::string_view kEorDetailsStats = "ui/flow/screen/eordetailsstatsscreen";
constexpr std::string_view kEorDetailsTabs = "ui/flow/screen/eordetailstabsscreen";
constexpr std::string_view kHud64 = "ui/flow/screen/scoreboards/scoreboardtwoteamshud64screen";
constexpr std::string_view kTwoTeams = "ui/flow/screen/scoreboards/scoreboardtwoteamsscreen";
constexpr std::string_view kStats = "instance1.Stats_01";
constexpr std::string_view kHeader = "instance1.EorHeader_01";
constexpr uint32_t kFriendlyText = 0x73C3FFu;
constexpr uint32_t kEnemyText = 0x963232u;

template <size_t N> void set_text(std::array<char, N> &r_out, std::string_view p_text) {
	r_out.fill('\0');
	std::copy_n(p_text.data(), std::min(p_text.size(), r_out.size() - 1), r_out.data());
}

HudModel footage_round(bool p_won) {
	HudModel m{};
	set_text(m.settings.current_game_mode, "TeamDeathMatch0");
	HudEndOfRound &e = m.end_of_round;
	e.visible = true;
	e.local_won = p_won;
	e.local_player_in_winning_team = p_won;
	e.team_results = { 96, 100 };
	e.player_team_index = 1;
	e.round_duration = 266.4f;
	return m;
}

HudModel footage_stats() {
	HudModel m = footage_round(false);
	HudEndOfRound &e = m.end_of_round;
	e.total_score = 1642;
	e.combat_score = 1642;
	e.kills = 14;
	e.deaths = 9;
	e.kill_death_ratio = 14.0f / 9.0f;
	e.kills_per_minute = 14.0f / (266.4f / 60.0f);
	e.longest_streak = 5;
	e.longest_headshot = 31.2f;
	e.score_per_minute = 1642.0f;
	return m;
}

std::string row(std::string_view p_row, std::string_view p_side) {
	return std::string(kStats) + "." + std::string(p_row) + "." + std::string(p_side);
}

std::string text_under(const RetailScreen &p_s, std::string_view p_widget) {
	for (SwfNodeId id = 0; id < 200000; ++id) {
		const SwfSceneNode *n = p_s.scene->node(id);
		if (n != nullptr && n->path.find(p_widget) != std::string::npos && n->path.ends_with(".txtDisplay")) {
			return p_s.hud->clip(n->path).text();
		}
	}
	return "no txtDisplay under " + std::string(p_widget);
}

std::pair<float, float> stage_text_span(const Retail &p_r, const RetailScreen &p_s, std::string_view p_widget) {
	for (SwfNodeId id = 0; id < 200000; ++id) {
		const SwfSceneNode *n = p_s.scene->node(id);
		if (n != nullptr && n->path.find(p_widget) != std::string::npos && n->path.ends_with(".txtDisplay")) {
			level::WorkBudget budget(kSwfTextBudgetUnits);
			const Result<SwfTextLayout> l = swf_layout_node_text(*p_r.fonts, *n, budget);
			if (!l || l->lines.empty()) {
				return { 0.0f, 0.0f };
			}
			const SwfTextLine &line = l->lines.front();
			const SwfMatrix &w = n->world;
			const float y = line.baseline;
			return { w.a * line.x + w.c * y + w.tx, w.a * (line.x + line.width) + w.c * y + w.tx };
		}
	}
	return { 0.0f, 0.0f };
}

HudModel scoreboard_round() {
	HudModel m = footage_round(false);
	const char *names[] = { "BOT_Sgt. Foster", "BOT_Sgt. Morgan", "Maggy", "BOT_Capt. Zorin" };
	const int16_t kills[] = { 6, 9, 7, 7 };
	for (uint8_t i = 0; i < 4; ++i) {
		HudPlayer &p = m.players[i];
		set_text(p.name, names[i]);
		p.team = i < 3 ? 1 : 2;
		p.bot = i != 2;
		p.local = i == 2;
		p.kills = kills[i];
		p.deaths = int16_t(i + 2);
		p.score = 1040 - 50 * i;
		p.ping_ms = 1;
	}
	m.local_player = 2;
	m.team.friendly_team_players.count = 3;
	m.team.friendly_team_players.index = { 0, 1, 2 };
	m.team.enemy_team1_players.count = 1;
	m.team.enemy_team1_players.index = { 3 };
	return m;
}

std::string clip(std::string_view p_path) {
	return std::string(kHeader) + "." + std::string(p_path);
}

float right_of(const As2Clip &p_field, float p_origin) {
	return p_origin + p_field.x() + p_field.text_width() + 4.0f;
}

}

TEST_SUITE("ui-eor") {
	TEST_CASE("hud_eor_round_duration formats the round as mm:ss and hides a zero round") {
		CHECK(hud_eor_round_duration(266.4f) == "04:26");
		CHECK(hud_eor_round_duration(267.0f) == "04:27");
		CHECK(hud_eor_round_duration(59.9f) == "00:59");
		CHECK(hud_eor_round_duration(0.0f).empty());
		CHECK(hud_eor_round_duration(std::nanf("")).empty());
		HudEndOfRound e;
		e.local_player_in_winning_team = true;
		CHECK(hud_eor_place(e) == 0);
		e.local_player_in_winning_team = false;
		CHECK(hud_eor_place(e) == 1);
	}

	TEST_CASE("EorHeader on eorwinningteamscreen: YOUR TEAM WON! or LOST! alone, with no team results or duration "
			  "bound") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		for (const bool won : { true, false }) {
			RetailScreen s =
					open_retail_screen(*r, kEorWinningTeamScreenPartition, retail_text_client(retail_text_db(*r)));
			for (const std::string &u : s.hud->stats().unknown_class_paths) {
				CHECK_MESSAGE(false, u);
			}
			s.hud->initialize(footage_round(won));
			CHECK(s.hud->clip(clip("headerText")).text() == (won ? "YOUR TEAM WON!" : "YOUR TEAM LOST!"));
			CHECK(s.at(kHeader).visible);
			CHECK_FALSE(s.hud->clip(clip("textHolder.values_0")).exists());
			CHECK_FALSE(s.at(clip("textHolder.durationText")).visible);
			CHECK_FALSE(s.at(clip("textHolder.durationDescText")).visible);
		}
	}

	TEST_CASE("EorHeader on eordetailsscoreboardscreen: ROUND DURATION mm:ss, then US KILLS and RU KILLS pairs in the "
			  "local team's friendly colour and the other's enemy colour, the holder centred on 640") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kEorDetailsScoreboard, retail_text_client(retail_text_db(*r)));
		for (const std::string &u : s.hud->stats().unknown_class_paths) {
			CHECK_MESSAGE(false, u);
		}
		s.hud->initialize(footage_round(false));
		CHECK(s.hud->clip(clip("headerText")).text() == "YOUR TEAM LOST!");
		const As2Clip desc = s.hud->clip(clip("textHolder.durationDescText"));
		const As2Clip duration = s.hud->clip(clip("textHolder.durationText"));
		CHECK(desc.text() == "ROUND DURATION:");
		CHECK(duration.text() == "04:26");
		CHECK(desc.visible());
		CHECK(duration.visible());
		const As2Clip us = s.hud->clip(clip("textHolder.values_0"));
		const As2Clip ru = s.hud->clip(clip("textHolder.values_1"));
		REQUIRE(us.exists());
		REQUIRE(ru.exists());
		CHECK(us.child("keyText").text() == "US KILLS:");
		CHECK(us.child("valueText").text() == "96");
		CHECK(ru.child("keyText").text() == "RU KILLS:");
		CHECK(ru.child("valueText").text() == "100");
		CHECK(us.child("keyText").text_color() == std::optional<uint32_t>(kFriendlyText));
		CHECK(us.child("valueText").text_color() == std::optional<uint32_t>(kFriendlyText));
		CHECK(ru.child("keyText").text_color() == std::optional<uint32_t>(kEnemyText));
		CHECK(ru.child("valueText").text_color() == std::optional<uint32_t>(kEnemyText));
		REQUIRE(s.at(us.child("keyText").path()).filters.size() == 1);
		CHECK(s.at(us.child("keyText").path()).filters[0].kind == SwfFilterKind::Glow);
		CHECK(duration.x() > right_of(desc, 0.0f));
		CHECK(us.x() > right_of(duration, 0.0f));
		CHECK(ru.x() == doctest::Approx(right_of(us.child("valueText"), us.x()) + 12.0f));
		CHECK(us.child("valueText").x() == doctest::Approx(right_of(us.child("keyText"), 0.0f) + 6.0f));
		const As2Clip holder = s.hud->clip(clip("textHolder"));
		const float width = right_of(ru.child("valueText"), ru.x()) - desc.x();
		CHECK(holder.x() == doctest::Approx(std::floor(640.0f - width / 2.0f + 0.5f)));
	}

	TEST_CASE("Stats on eordetailsstatsscreen: the footage's end-of-round rows, the dimmed unset best vehicle and no "
			  "rank "
			  "bar off a ranked server") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kEorDetailsStats, retail_text_client(retail_text_db(*r)));
		CHECK(s.hud->stats().unknown_class_paths.empty());
		s.hud->initialize(footage_stats());
		CHECK(s.at(kStats).visible);
		const auto text = [&](std::string_view p_row, std::string_view p_side) {
			return s.hud->clip(row(p_row, p_side)).text();
		};
		CHECK(text("totalPlayTime_field", "leftText") == "TOTAL PLAY TIME");
		CHECK(text("totalPlayTime_field", "rightText") == "0:00:00");
		CHECK(text("awardScore_field", "rightText") == "0");
		CHECK(text("totalScore_field", "leftText") == "TOTAL SCORE");
		CHECK(text("totalScore_field", "rightText") == "1642");
		CHECK(text("SPM_field", "rightText") == "1642");
		CHECK(text("kills_field", "rightText") == "14");
		CHECK(text("deaths_field", "rightText") == "9");
		CHECK(text("kdRatio_field", "rightText") == "1.56");
		CHECK(text("KPM_field", "rightText") == "3.15");
		CHECK(text("HighestStreak_field", "rightText") == "5");
		CHECK(text("DogtagsTaken_field", "rightText") == "-");
		CHECK(text("LongestHeadShot_field", "rightText") == "31m");
		CHECK(text("VehicleKills", "rightText") == "0");
		CHECK_FALSE(s.at(std::string(kStats) + ".statsSince_field").visible);
		CHECK(s.hud->clip(std::string(kStats) + ".VehicleHeader").alpha() == doctest::Approx(50.0f));
		CHECK(s.hud->clip(std::string(kStats) + ".VehicleKills").alpha() == doctest::Approx(50.0f));
		CHECK_FALSE(s.at(std::string(kStats) + ".bar").visible);
		CHECK_FALSE(s.at(std::string(kStats) + ".playerNameText").visible);
	}

	TEST_CASE("hud_eor_play_time and hud_eor_distance format the STATS values as the footage shows them") {
		CHECK(hud_eor_play_time(0.0f) == "0:00:00");
		CHECK(hud_eor_play_time(3725.9f) == "1:02:05");
		CHECK(hud_eor_distance(0.0f) == "-");
		CHECK(hud_eor_distance(31.2f) == "31m");
		CHECK(hud_eor_distance(31.5f) == "32m");
	}

	TEST_CASE("eordetailstabsscreen: NEXT ROUND STARTS IN: and the whole seconds left until the level change") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kEorDetailsTabs, retail_text_client(retail_text_db(*r)));
		for (const std::string &u : s.hud->stats().unknown_class_paths) {
			CHECK_MESSAGE(false, u);
		}
		HudModel m = footage_round(false);
		m.end_of_round.time_until_level_change = 47.6f;
		s.hud->initialize(m);
		CHECK(text_under(s, "TextField_02") == "NEXT ROUND STARTS IN:");
		CHECK(text_under(s, "TextField_01") == "47");
		s.scene->take_changes();
		const std::pair<float, float> caption = stage_text_span(*r, s, "TextField_02");
		const std::pair<float, float> value = stage_text_span(*r, s, "TextField_01");
		MESSAGE("caption " << caption.first << ".." << caption.second << ", value " << value.first << ".."
						   << value.second);
		CHECK(caption.first > 700.0f);
		CHECK(caption.second < value.first);
		CHECK(value.second > 1150.0f);
		CHECK(value.second < 1190.0f);
		m.end_of_round.time_until_level_change = 0.4f;
		s.hud->refresh(m);
		CHECK(text_under(s, "TextField_01") == "0");
		CHECK(hud_eor_seconds_left(-1.0f) == 0);
		CHECK(hud_eor_seconds_left(std::nanf("")) == 0);
	}

	TEST_CASE("The EOR tabs: END OF ROUND over the page, and off a ranked server only SCOREBOARD and STATS") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen tabs = open_retail_screen(*r, kEorDetailsTabs, retail_text_client(retail_text_db(*r)));
		tabs.hud->initialize(footage_round(false));
		const std::string header = std::string(kHudScreenInstance) + ".PageHeader_01";
		CHECK(tabs.hud->clip(header + ".subHeading").text() == "END OF ROUND");
		CHECK(tabs.hud->clip(header + ".heading").text().empty());
		const std::string holder = std::string(kHudScreenInstance) + ".TabBar_01.tabBarHolder";
		for (const bool ranked : { false, true }) {
			RetailScreen s = open_retail_screen(*r, kEorDetailsScoreboard, retail_text_client(retail_text_db(*r)));
			HudModel m = footage_round(false);
			m.end_of_round.is_ranked = ranked;
			s.hud->initialize(m);
			CHECK(s.hud->clip(holder + ".button_1").exists());
			CHECK(s.hud->clip(holder + ".button_2").exists() == ranked);
			CHECK(s.hud->clip(holder + ".button_6").exists() == ranked);
		}
	}

	TEST_CASE("hud_scoreboard_rows: one row per listed player, bots without a ping, the local player marked") {
		const HudModel m = scoreboard_round();
		const std::vector<As2Value> us = hud_scoreboard_rows(m, m.team.friendly_team_players);
		REQUIRE(us.size() == 3);
		CHECK(us[0].field("Label").to_string() == "BOT_Sgt. Foster");
		CHECK(us[0].field("Ping").to_string() == "-");
		CHECK(us[2].field("Label").to_string() == "Maggy");
		CHECK(us[2].field("Ping").to_string() == "1");
		CHECK(us[2].field("LocalPlayer").truthy());
		CHECK_FALSE(us[0].field("LocalPlayer").truthy());
		CHECK(us[1].field("Kills").to_string() == "9");
		CHECK(us[1].field("Score").to_string() == "990");
		CHECK(hud_scoreboard_rows(m, m.team.enemy_team1_players).size() == 1);
	}

	TEST_CASE("The EOR scoreboards take the team rows: hud64's 20 px rows and the EOR page's two 12-row lists") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const HudModel m = scoreboard_round();
		for (const std::string_view partition : { kHud64, kTwoTeams }) {
			RetailScreen s = open_retail_screen(*r, partition, retail_text_client(retail_text_db(*r)));
			for (const std::string &u : s.hud->stats().unknown_class_paths) {
				CHECK_MESSAGE(false, u);
			}
			s.hud->initialize(m);
			REQUIRE(hud_list_set_direct_rows(*s.hud, "List_01", hud_scoreboard_rows(m, m.team.friendly_team_players)));
			REQUIRE(hud_list_set_direct_rows(*s.hud, "List_02", hud_scoreboard_rows(m, m.team.enemy_team1_players)));
			s.hud->refresh(m);
			CHECK_MESSAGE(s.hud->stats().as2.failed_ops == 0, s.hud->stats().as2.last_failure);
			bool foster = false;
			bool zorin = false;
			for (SwfNodeId id = 0; id < 200000; ++id) {
				const SwfSceneNode *n = s.scene->node(id);
				if (n == nullptr || !n->path.ends_with(".Label")) {
					continue;
				}
				const std::string t = s.hud->clip(n->path).text();
				foster = foster || (t == "BOT_Sgt. Foster" && n->path.find("List_01") != std::string::npos);
				zorin = zorin || (t == "BOT_Capt. Zorin" && n->path.find("List_02") != std::string::npos);
			}
			CHECK_MESSAGE(foster, partition);
			CHECK_MESSAGE(zorin, partition);
			if (partition == kHud64) {
				for (const std::string_view header : { "ScoreboardHeader_01", "ScoreboardHeader_02" }) {
					const As2Clip tickets =
							s.hud->clip(std::string(kHudScreenInstance) + "." + std::string(header) + ".ticketText");
					CHECK_MESSAGE(tickets.exists(), header);
					CHECK_MESSAGE(!tickets.visible(), header);
				}
			}
		}
	}

	TEST_CASE("EorHeader colours the RU pair friendly when the local team is RU") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		RetailScreen s = open_retail_screen(*r, kEorDetailsScoreboard, retail_text_client(retail_text_db(*r)));
		HudModel m = footage_round(true);
		m.end_of_round.player_team_index = 2;
		s.hud->initialize(m);
		CHECK(s.hud->clip(clip("headerText")).text() == "YOUR TEAM WON!");
		CHECK(s.hud->clip(clip("textHolder.values_0.keyText")).text_color() == std::optional<uint32_t>(kEnemyText));
		CHECK(s.hud->clip(clip("textHolder.values_1.keyText")).text_color() == std::optional<uint32_t>(kFriendlyText));
	}
}
