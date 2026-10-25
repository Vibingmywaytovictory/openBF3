#include "hud_feed_tdm_parts.h"

#include "bf3/core/ui/text_db.h"
#include "bf3/host/kits.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace bf3::host::tdm_feed {

namespace ui = bf3::core::ui;

namespace {

constexpr std::string_view kTeam1NameSid = "ID_M_US";
constexpr std::string_view kTeam2NameSid = "ID_M_RU";

bool rank_before(const Rank &p_a, const Rank &p_b) {
	if (p_a.score != p_b.score) {
		return p_a.score > p_b.score;
	}
	if (p_a.kills != p_b.kills) {
		return p_a.kills > p_b.kills;
	}
	return p_a.slot < p_b.slot;
}

void set_list(ui::HudPlayerList &r_list, std::span<const Rank> p_ranks) {
	r_list = {};
	for (const Rank &r : p_ranks) {
		if (r_list.count >= r_list.index.size()) {
			break;
		}
		r_list.index[r_list.count++] = r.slot;
	}
}

void copy_name(const ui::HudPlayer &p_player, ui::HudPlayerName &r_name) {
	r_name = p_player.name;
}

void feed_players(Frame &r_frame) {
	const HudFeedInput &in = r_frame.input;
	ui::HudModel &model = r_frame.model;
	HudTdmFeedState &state = r_frame.state;
	model.players.fill(ui::HudPlayer{});
	size_t highest = 0;
	for (const net::PlayerRow &row : in.players) {
		if (row.slot >= ui::kHudMaxPlayers) {
			++state.bad_inputs;
			continue;
		}
		ui::HudPlayer &p = model.players[row.slot];
		p = ui::HudPlayer{};
		const size_t bytes = std::min<size_t>(std::min<size_t>(row.name_bytes, row.name.size()), p.name.size() - 1);
		if (row.name_bytes > row.name.size()) {
			++state.bad_inputs;
		}
		std::memcpy(p.name.data(), row.name.data(), bytes);
		if (row.team > 2) {
			++state.bad_inputs;
		} else {
			p.team = int8_t(row.team);
		}
		if (row.kit >= kKitCount) {
			++state.bad_inputs;
		} else {
			p.kit = int8_t(row.kit);
			if (in.kits != nullptr && row.team >= 1 && row.team <= kKitTeams) {
				p.kit_icon = int16_t(std::clamp<int32_t>(in.kits->loadout(row.team, row.kit).hud_icon, 0, 32767));
			}
		}
		p.alive = row.alive;
		p.bot = row.bot;
		p.local = row.slot == r_frame.own_slot;
		p.kills = int16_t(std::min<uint32_t>(row.kills, 32767u));
		p.deaths = int16_t(std::min<uint32_t>(row.deaths, 32767u));
		p.score = row.score;
		p.ping_ms = row.ping_ms;
		r_frame.present[row.slot] = true;
		highest = std::max<size_t>(highest, size_t(row.slot) + 1);
	}
	model.player_count = uint8_t(highest);
	model.local_player = r_frame.own_slot;
	model.local_team = int8_t(r_frame.own_team);
}

}

void feed_scoreboard(Frame &r_frame) {
	feed_players(r_frame);
	ui::HudModel &model = r_frame.model;
	std::array<Rank, ui::kHudMaxPlayers> t1{};
	std::array<Rank, ui::kHudMaxPlayers> t2{};
	size_t n_all = 0;
	size_t n1 = 0;
	size_t n2 = 0;
	for (size_t s = 0; s < ui::kHudMaxPlayers; ++s) {
		if (!r_frame.present[s]) {
			continue;
		}
		const ui::HudPlayer &p = model.players[s];
		const Rank r{ uint8_t(s), p.score, p.kills };
		r_frame.ranked[n_all++] = r;
		if (p.team == 1) {
			t1[n1++] = r;
		} else if (p.team == 2) {
			t2[n2++] = r;
		}
	}
	r_frame.ranked_count = n_all;
	std::sort(r_frame.ranked.begin(), r_frame.ranked.begin() + n_all, rank_before);
	std::sort(t1.begin(), t1.begin() + n1, rank_before);
	std::sort(t2.begin(), t2.begin() + n2, rank_before);
	const uint8_t my_team = r_frame.my_team;
	const uint8_t enemy_team = r_frame.enemy_team;
	ui::HudTeam &team = model.team;
	team.active_teams_data = 2;
	set_list(team.sorted_players, std::span<const Rank>(r_frame.ranked.data(), n_all));
	set_list(team.team1_players, std::span<const Rank>(t1.data(), n1));
	set_list(team.team2_players, std::span<const Rank>(t2.data(), n2));
	team.friendly_team_players = my_team == 1 ? team.team1_players : team.team2_players;
	team.enemy_team1_players = my_team == 1 ? team.team2_players : team.team1_players;
	team.team1_name = ui::text_hash(kTeam1NameSid);
	team.team2_name = ui::text_hash(kTeam2NameSid);
	team.friendly_team = my_team;
	team.enemy_team1 = enemy_team;
	team.friendly_team_name = my_team == 1 ? team.team1_name : team.team2_name;
	team.enemy_team1_name = my_team == 1 ? team.team2_name : team.team1_name;
	ui::HudPlayerName *const tops[3] = { &team.top_player1_name, &team.top_player2_name, &team.top_player3_name };
	for (size_t i = 0; i < 3; ++i) {
		*tops[i] = {};
		if (i < n_all) {
			copy_name(model.players[r_frame.ranked[i].slot], *tops[i]);
		}
	}
	const uint8_t winner = r_frame.winner;
	team.friendly_team_is_winner = r_frame.round_over && winner != 0 && winner == my_team;
	team.enemy_team1_is_winner = r_frame.round_over && winner != 0 && winner == enemy_team;
	team.scoreboard_enabled = r_frame.input.scoreboard_held;

	ui::HudKillCounter &kc = model.kill_counter;
	const int32_t k1 = r_frame.team1_kills;
	const int32_t k2 = r_frame.team2_kills;
	kc.team1_kills = k1;
	kc.team2_kills = k2;
	kc.team_kills_list = {};
	kc.team_kills_list.count = 2;
	kc.team_kills_list.kills[0] = k1;
	kc.team_kills_list.kills[1] = k2;
	kc.team1_max_kills = r_frame.kill_limit;
	kc.team2_max_kills = r_frame.kill_limit;
	const int32_t mine = my_team == 1 ? k1 : k2;
	const int32_t theirs = my_team == 1 ? k2 : k1;
	kc.player_team = my_team;
	kc.player_team_kills = mine;
	kc.enemy_team_kills = theirs;
	kc.leading_team_kills = std::max(k1, k2);
	kc.leading_by_kills = mine - theirs;
	kc.player_team_position = mine >= theirs ? 1 : 2;
}

}
