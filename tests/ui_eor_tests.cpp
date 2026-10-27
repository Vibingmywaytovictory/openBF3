#include <doctest/doctest.h>

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_scene.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using namespace ui_retail;

namespace {

constexpr std::string_view kEorDetailsScoreboard = "ui/flow/screen/eordetailsscoreboardscreen";
constexpr std::string_view kEorDetailsStats = "ui/flow/screen/eordetailsstatsscreen";
constexpr std::string_view kStats = "instance1.Stats_01";
constexpr std::string_view kHeader = "instance1.EorHeader_01";
constexpr uint32_t kFriendlyText = 0x73C3FFu;
constexpr uint32_t kEnemyText = 0x963232u;

void set_text(std::array<char, kHudTextBytes> &r_out, std::string_view p_text) {
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
