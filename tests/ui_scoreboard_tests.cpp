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
}
