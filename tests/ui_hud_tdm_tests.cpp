#include <doctest/doctest.h>

#include "test_support.h"

#include "hud_tdm_text.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/eor_timeline.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/swf_atlas.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/ui/ui_fonts.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/kits.h"
#include "bf3/host/presentation_events.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_install.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ui = bf3::core::ui;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;
namespace host = bf3::host;
namespace net = bf3::net;
namespace gs = bf3::godot_side;

namespace {

void row(const std::string &p_name, bool p_pass, const std::string &p_detail) {
	std::printf("TDM ROW %s %s %s\n", p_name.c_str(), p_pass ? "PASS" : "FAIL", p_detail.c_str());
	std::fflush(stdout);
}

net::PlayerRow player(uint8_t p_slot, uint8_t p_team, const char *p_name, int32_t p_score = 0, uint16_t p_kills = 0, bool p_bot = true) {
	net::PlayerRow r;
	r.slot = p_slot;
	r.team = p_team;
	r.bot = p_bot;
	r.alive = true;
	r.score = p_score;
	r.kills = p_kills;
	const size_t n = std::min(std::strlen(p_name), r.name.size());
	std::memcpy(r.name.data(), p_name, n);
	r.name_bytes = uint8_t(n);
	return r;
}

std::string name_of(const ui::HudPlayerName &p_name) {
	return std::string(p_name.data(), size_t(std::find(p_name.begin(), p_name.end(), '\0') - p_name.begin()));
}

host::HudStatic make_static() {
	host::HudStatic st;
	for (uint32_t d = 0; d < 6; ++d) {
		host::HudStaticRow r;
		r.item_id = 1000 + d;
		r.name_sid_hash = 2000 + d;
		st.rows.push_back(r);
	}
	return st;
}

host::PresentEvent death(uint8_t p_killer, uint8_t p_victim, uint16_t p_def, uint8_t p_flags = 0, bf3::sim::Tick p_tick = 0) {
	host::PresentEvent e;
	e.kind = host::PresentKind::Death;
	e.timeline = host::PresentTimeline::Render;
	e.slot = p_killer;
	e.aux = p_victim;
	e.def = p_def;
	e.flags = p_flags;
	e.time = sw::tick_end(p_tick);
	return e;
}

host::PresentEvent spawn_event(uint8_t p_slot) {
	host::PresentEvent e;
	e.kind = host::PresentKind::Spawn;
	e.timeline = host::PresentTimeline::Predicted;
	e.slot = p_slot;
	return e;
}

}

TEST_SUITE("ui-hud-tdm-unit") {

TEST_CASE("a recorded round: pre-round, deploy, kills, own death, kill card, spawn screen, round end, restart") {
	const host::HudStatic st = make_static();
	std::vector<net::PlayerRow> rows{ player(1, 2, "Bot RU 1", 300, 3), player(2, 1, "Bot US 1", 200, 2), player(3, 1, "Me", 0, 0, false),
		player(4, 2, "Bot RU 2", 100, 1), player(6, 2, "Bot RU 3", 300, 3), player(9, 1, "Bot US 2", 500, 5) };
	host::HudFeedInput in;
	in.tdm = true;
	in.match_valid = true;
	in.match.phase = net::MatchPhase::PreRound;
	in.match.kill_limit = 100;
	in.match.countdown_end_tick = 600;
	in.match.restrict_until_tick = 600;
	in.players = rows;
	in.own_slot = 3;
	in.own_team = 1;
	in.alive = false;
	in.own_unspawned = false;
	in.spawned_this_round = false;
	in.spawn_kit = 2;
	host::HudTdmFeedState state;
	ui::HudModel m;
	const double dt = 1.0 / 60.0;
	uint64_t frames = 0;
	uint64_t deaths_sent = 0;
	uint64_t mismatches = 0;
	const auto frame = [&](bf3::sim::Tick p_tick, std::span<const host::PresentEvent> p_events) {
		in.server_tick = p_tick;
		in.now_s = double(p_tick) * dt;
		const uint64_t serial = m.serial;
		host::fill_hud_tdm(in, p_events, st, state, m);
		++frames;
		for (const host::PresentEvent &e : p_events) {
			deaths_sent += e.kind == host::PresentKind::Death ? 1 : 0;
		}

		bool ok = m.serial == serial;
		ok = ok && m.kill_counter.team1_kills == in.match.kills[0] && m.kill_counter.team2_kills == in.match.kills[1];
		ok = ok && m.kill_counter.team1_max_kills == in.match.kill_limit;
		ok = ok && m.messages.kill_message.newest_serial == deaths_sent && state.deaths_fed == deaths_sent;
		ok = ok && m.pre_round.visible == (in.match.phase == net::MatchPhase::PreRound);
		ok = ok && m.end_of_round.visible == (in.match.phase == net::MatchPhase::RoundOver);
		ok = ok && !(m.spawn_screen.visible && m.kill_card.visible) && !(m.spawn_screen.visible && m.end_of_round.visible);
		ok = ok && (!m.spawn_screen.visible || !in.alive || in.own_unspawned);
		ok = ok && m.local_player == 3 && m.players[3].local && m.player_count == 10;
		if (!ok) {
			++mismatches;
		}
	};

	frame(100, {});
	CHECK(m.pre_round.visible);
	CHECK(m.pre_round.counting_down);
	CHECK(m.pre_round.seconds_left == doctest::Approx(500.0f / 60.0f));
	CHECK(m.pre_round.input_restricted);
	CHECK(m.spawn_screen.visible);
	CHECK(m.spawn_screen.at_join);
	CHECK(m.spawn_screen.can_deploy);
	CHECK(m.spawn_screen.seconds_to_deploy == 0.0f);
	CHECK(!m.spawn_screen.revive_possible);
	CHECK(m.spawn_screen.selected_kit == 2);
	CHECK(!m.kill_card.visible);
	CHECK(m.team.friendly_team == 1);
	CHECK(m.team.enemy_team1 == 2);

	in.alive = true;
	in.spawned_this_round = true;
	const host::PresentEvent own_spawn = spawn_event(3);
	frame(200, std::span<const host::PresentEvent>(&own_spawn, 1));
	CHECK(!m.spawn_screen.visible);
	CHECK(m.pre_round.input_restricted);

	in.match.phase = net::MatchPhase::InRound;
	in.match.countdown_end_tick = 0;
	frame(600, {});
	CHECK(!m.pre_round.visible);
	CHECK(!m.pre_round.input_restricted);

	const std::array<host::PresentEvent, 4> kills{ death(1, 2, 0, host::present_flags::kHeadshot, 700), death(9, 4, 1, 0, 701), death(4, 6, 2, 0, 702),
		death(6, 6, sw::kNoWeapon, 0, 703) };
	in.match.kills = { 1, 1 };
	frame(705, kills);
	CHECK(m.messages.kill_message.newest_serial == 4);
	const ui::HudKill &hs = m.messages.kill_message.lines[1 % ui::kHudKillFeedSize];
	CHECK(hs.serial == 1);
	CHECK(hs.killer == 1);
	CHECK(hs.victim == 2);
	CHECK(hs.item_id == 1000);
	CHECK(hs.weapon_name_sid_hash == 2000);
	CHECK(hs.headshot);
	CHECK(!hs.team_kill);
	CHECK(m.messages.kill_message.lines[3].team_kill);
	const ui::HudKill &fall = m.messages.kill_message.lines[4];
	CHECK(fall.suicide);
	CHECK(fall.item_id == 0);
	CHECK(fall.weapon_name_sid_hash == 0);
	CHECK(m.messages.local_kill_message.newest_serial == 0);
	CHECK(state.unknown_defs == 0);

	const host::PresentEvent own_kill = death(3, 1, 3, host::present_flags::kOwnShooter, 900);
	in.match.kills = { 2, 1 };
	frame(901, std::span<const host::PresentEvent>(&own_kill, 1));
	CHECK(m.messages.local_kill_message.newest_serial == 1);
	CHECK(m.messages.local_kill_message.lines[1].victim == 1);
	CHECK(m.kill_counter.player_team_kills == 2);
	CHECK(m.kill_counter.enemy_team_kills == 1);
	CHECK(m.kill_counter.leading_by_kills == 1);
	CHECK(m.kill_counter.player_team_position == 1);

	rows[4].kit = 2;
	host::PresentEvent own_death = death(6, 3, 4, host::present_flags::kOwnVictim | host::present_flags::kHeadshot, 1000);
	own_death.value = 37.5f;
	own_death.extra = 1300;
	own_death.seed = 1780;
	in.alive = false;
	in.own_respawn_tick = 1300;
	in.own_revive_until = 1780;
	in.kill_card_source.killer_seen = true;
	in.kill_card_source.distance_m = 23.4f;
	in.match.kills = { 2, 2 };
	frame(1002, std::span<const host::PresentEvent>(&own_death, 1));
	CHECK(!m.kill_card.visible);
	CHECK(m.kill_card.serial == 1);
	CHECK(m.kill_card.killer == 6);
	CHECK(m.kill_card.killer_kit == 2);
	CHECK(m.kill_card.headshot);
	CHECK(!m.kill_card.suicide);
	CHECK(m.kill_card.item_id == 1004);
	CHECK(m.kill_card.weapon_name_sid_hash == 2004);
	CHECK(m.kill_card.killer_health == doctest::Approx(37.5f));
	CHECK(m.kill_card.distance_m == doctest::Approx(23.4f));
	CHECK(m.kill_card.seconds_left == 0.0f);
	CHECK(!m.spawn_screen.visible);
	CHECK(m.soldier.show_medic_revive);
	CHECK(m.spawn_logic.start_time_to_respawn == doctest::Approx(5.0f));

	in.kill_card_source = {};
	const double card_from = 1002 * dt + host::kKillCardDelaySeconds;
	const double card_until = card_from + host::kKillCardSeconds;
	uint32_t card_frames = 0;
	uint32_t expected_frames = 0;
	uint32_t early_screens = 0;
	float first_seconds_left = -1.0f;
	bf3::sim::Tick t = 1003;
	for (; t < 1002 + 4 * 60 + 2; ++t) {
		frame(t, {});
		const double now = double(t) * dt;
		expected_frames += now >= card_from && now < card_until ? 1 : 0;
		card_frames += m.kill_card.visible ? 1 : 0;
		early_screens += now < card_until && m.spawn_screen.visible ? 1 : 0;
		if (m.kill_card.visible && first_seconds_left < 0.0f) {
			first_seconds_left = m.kill_card.seconds_left;
		}
	}
	CHECK(card_frames == expected_frames);
	CHECK(expected_frames >= 3 * 60 - 1);
	CHECK(expected_frames <= 3 * 60 + 1);
	CHECK(early_screens == 0);
	CHECK(first_seconds_left == doctest::Approx(host::kKillCardSeconds).epsilon(dt / host::kKillCardSeconds));
	CHECK(!m.kill_card.visible);
	CHECK(m.spawn_screen.visible);
	CHECK(!m.spawn_screen.at_join);
	CHECK(!m.spawn_screen.can_deploy);
	CHECK(m.spawn_screen.seconds_to_deploy == doctest::Approx(float(1300 - (t - 1)) / 60.0f));
	CHECK(m.spawn_screen.revive_possible);
	CHECK(m.spawn_screen.seconds_revive_left == doctest::Approx(float(1780 - (t - 1)) / 60.0f));
	CHECK(m.spawn_logic.time_to_respawn == m.spawn_screen.seconds_to_deploy);

	frame(1299, {});
	CHECK(!m.spawn_screen.can_deploy);
	frame(1300, {});
	CHECK(m.spawn_screen.can_deploy);
	CHECK(m.spawn_screen.seconds_to_deploy == 0.0f);

	frame(1780, {});
	CHECK(!m.spawn_screen.revive_possible);
	CHECK(!m.soldier.show_medic_revive);

	in.alive = true;
	frame(1790, std::span<const host::PresentEvent>(&own_spawn, 1));
	CHECK(!m.spawn_screen.visible);

	host::PresentEvent own_death2 = death(1, 3, 0, host::present_flags::kOwnVictim, 2000);
	own_death2.extra = 2300;
	own_death2.seed = 2780;
	in.alive = false;
	in.own_respawn_tick = 2300;
	in.own_revive_until = 2780;
	frame(2001, std::span<const host::PresentEvent>(&own_death2, 1));
	CHECK(!m.kill_card.visible);
	frame(2001 + 60, {});
	CHECK(m.kill_card.visible);
	CHECK(m.kill_card.serial == 2);
	CHECK(m.kill_card.distance_m < 0.0f);
	in.alive = true;
	frame(2100, std::span<const host::PresentEvent>(&own_spawn, 1));
	CHECK(!m.kill_card.visible);
	CHECK(!m.spawn_screen.visible);

	in.match.phase = net::MatchPhase::RoundOver;
	in.match.kills = { 97, 100 };
	in.match.winner = 2;
	in.match.round_end_tick = 9000;
	in.match.restart_tick = 9000 + 3600;
	in.alive = false;
	frame(9001, {});
	CHECK(m.end_of_round.visible);
	CHECK(m.end_of_round.winner == 2);
	CHECK(!m.end_of_round.local_won);
	CHECK(m.end_of_round.team_kills == std::array<int32_t, 2>{ 97, 100 });
	CHECK(m.end_of_round.kill_limit == 100);
	CHECK(m.end_of_round.seconds_to_next_round == doctest::Approx(3599.0f / 60.0f));
	CHECK(m.end_of_round.top_players == std::array<uint8_t, 3>{ 9, 1, 6 });
	CHECK(m.team.enemy_team1_is_winner);
	CHECK(!m.team.friendly_team_is_winner);
	CHECK(!m.spawn_screen.visible);
	CHECK(!m.pre_round.visible);

	in.match = {};
	in.match.phase = net::MatchPhase::PreRound;
	in.match.kill_limit = 100;
	in.match.countdown_end_tick = 0;
	in.match.players_needed = 2;
	in.spawned_this_round = false;
	in.own_respawn_tick = 0;
	in.own_revive_until = 0;
	frame(12600, {});
	CHECK(m.pre_round.visible);
	CHECK(!m.pre_round.counting_down);
	CHECK(m.pre_round.players_needed == 2);
	CHECK(m.pre_round.count_left == 2);
	in.match.players_needed = 0;
	in.match.countdown_end_tick = 12600 + 600;
	frame(12601, {});
	CHECK(m.pre_round.count_left == 0);
	CHECK(!m.end_of_round.visible);
	CHECK(m.pre_round.visible);
	CHECK(m.spawn_screen.visible);
	CHECK(m.spawn_screen.at_join);
	CHECK(m.kill_counter.team1_kills == 0);
	CHECK(m.end_of_round.winner == 0);
	CHECK(state.own_deaths == 2);
	CHECK(state.bad_inputs == 0);
	CHECK(mismatches == 0);
	row("unit.round", mismatches == 0 && state.bad_inputs == 0,
			std::to_string(frames) + " frames, " + std::to_string(deaths_sent) + " deaths = " + std::to_string(state.deaths_fed) + " feed lines, " +
					std::to_string(state.own_deaths) + " kill cards (" + std::to_string(card_frames) + " frames for 4 s), " + std::to_string(mismatches) +
					" mismatching frames");
}

TEST_CASE("players by slot and the sorted team lists") {
	const host::HudStatic st;

	std::vector<net::PlayerRow> rows{ player(40, 2, "Forty", 100, 1), player(5, 1, "Five", 200, 2), player(7, 2, "Seven", 200, 2),
		player(2, 1, "Two", 200, 3), player(63, 0, "Spectator", 900, 9), player(11, 2, "Me", 50, 0, false) };
	rows[1].kit = 3;
	rows[1].deaths = 4;
	rows[1].ping_ms = 35;
	rows[1].alive = false;
	host::HudFeedInput in;
	in.tdm = true;
	in.match_valid = true;
	in.match.phase = net::MatchPhase::InRound;
	in.match.kills = { 10, 20 };
	in.match.kill_limit = 50;
	in.players = rows;
	in.own_slot = 11;
	in.own_team = 2;
	in.alive = true;
	in.scoreboard_held = true;
	host::HudTdmFeedState state;
	ui::HudModel m;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(m.player_count == 64);
	CHECK(name_of(m.players[5].name) == "Five");
	CHECK(m.players[5].kit == 3);
	CHECK(m.players[5].deaths == 4);
	CHECK(m.players[5].ping_ms == 35);
	CHECK(!m.players[5].alive);
	CHECK(m.players[5].bot);
	CHECK(!m.players[11].bot);
	CHECK(m.players[11].local);
	CHECK(m.players[12].name[0] == '\0');
	CHECK(m.local_team == 2);
	CHECK(m.team.scoreboard_enabled);
	CHECK(m.team.friendly_team == 2);
	CHECK(m.team.enemy_team1 == 1);
	REQUIRE(m.team.team1_players.count == 2);
	CHECK(m.team.team1_players.index[0] == 2);
	CHECK(m.team.team1_players.index[1] == 5);
	REQUIRE(m.team.team2_players.count == 3);
	CHECK(m.team.team2_players.index[0] == 7);
	CHECK(m.team.team2_players.index[1] == 40);
	CHECK(m.team.team2_players.index[2] == 11);
	CHECK(m.team.friendly_team_players.count == 3);
	CHECK(m.team.friendly_team_players.index[0] == 7);
	CHECK(m.team.enemy_team1_players.index[0] == 2);
	REQUIRE(m.team.sorted_players.count == 6);
	CHECK(m.team.sorted_players.index[0] == 63);
	CHECK(name_of(m.team.top_player1_name) == "Spectator");
	CHECK(name_of(m.team.top_player2_name) == "Two");
	CHECK(name_of(m.team.top_player3_name) == "Five");
	CHECK(m.team.team1_name == ui::text_hash("ID_M_US"));
	CHECK(m.team.friendly_team_name == ui::text_hash("ID_M_RU"));
	CHECK(m.kill_counter.player_team == 2);
	CHECK(m.kill_counter.player_team_kills == 20);
	CHECK(m.kill_counter.enemy_team_kills == 10);
	CHECK(m.kill_counter.leading_team_kills == 20);
	CHECK(m.kill_counter.team_kills_list.count == 2);
	CHECK(state.bad_inputs == 0);

	rows = std::vector<net::PlayerRow>{ rows[1] };
	in.players = rows;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(m.player_count == 6);
	CHECK(m.players[40].name[0] == '\0');
	CHECK(m.team.team2_players.count == 0);
	CHECK(m.team.friendly_team_players.count == 0);
}

TEST_CASE("the kill-feed ring keeps the newest 8; unknown defs are counted") {
	const host::HudStatic st = make_static();
	host::HudFeedInput in;
	in.tdm = true;
	in.match_valid = true;
	in.match.phase = net::MatchPhase::InRound;
	in.own_slot = 0;
	in.own_team = 1;
	in.alive = true;
	host::HudTdmFeedState state;
	ui::HudModel m;
	std::vector<host::PresentEvent> events;
	for (uint8_t i = 0; i < 20; ++i) {
		events.push_back(death(uint8_t(10 + i), uint8_t(40 + i), uint16_t(i % 7), i % 5 == 0 ? host::present_flags::kOwnShooter : uint8_t(0)));
	}
	host::fill_hud_tdm(in, events, st, state, m);
	CHECK(m.messages.kill_message.newest_serial == 20);
	for (uint32_t serial = 13; serial <= 20; ++serial) {
		const ui::HudKill &l = m.messages.kill_message.lines[serial % ui::kHudKillFeedSize];
		CHECK(l.serial == serial);
		CHECK(l.killer == 10 + (serial - 1));
		CHECK(l.victim == 40 + (serial - 1));
		const uint16_t def = uint16_t((serial - 1) % 7);
		CHECK(l.item_id == (def < 6 ? 1000u + def : 0u));
	}
	CHECK(state.unknown_defs == 2);
	CHECK(m.messages.local_kill_message.newest_serial == 4);
	CHECK(state.deaths_fed == 20);
	CHECK(state.own_deaths == 0);
	CHECK(!m.kill_card.visible);
}

TEST_CASE("round stats: a replayed kill log gives the kill matrix, streaks, nemesis, best weapon and the round-end "
		  "page") {
	host::HudStatic st = make_static();
	st.rows[5].weapon_class = host::hud_weapon_class("wcKnife");
	std::vector<net::PlayerRow> rows;
	for (uint8_t s = 0; s < 8; ++s) {
		rows.push_back(player(s, uint8_t(1 + s % 2), s == 2 ? "Me" : "Bot", int32_t(s) * 10, 0, s != 2));
	}
	rows[2].score = 1500;
	host::HudFeedInput in;
	in.tdm = true;
	in.match_valid = true;
	in.match.phase = net::MatchPhase::InRound;
	in.match.round_start_tick = 600;
	in.match.kill_limit = 100;
	in.players = rows;
	in.own_slot = 2;
	in.own_team = 1;
	in.alive = true;
	in.own_position = { 0.0f, 0.0f, 0.0f };
	std::vector<bf3::present::PresentedSoldier> others;
	for (uint8_t s = 0; s < 8; ++s) {
		bf3::present::PresentedSoldier p;
		p.slot = s;
		p.position = { float(s) * 3.0f, 0.0f, float(s) * 4.0f };
		others.push_back(p);
	}
	in.others = others;
	host::HudTdmFeedState state;
	ui::HudModel m;

	struct Kill {
		uint8_t killer;
		uint8_t victim;
		uint16_t def;
		bool headshot;
	};
	std::vector<Kill> log;
	uint64_t seed = 0x9E3779B97F4A7C15ull;
	const auto next = [&](uint32_t p_n) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		return uint32_t((seed >> 33) % p_n);
	};
	for (int i = 0; i < 400; ++i) {
		const uint8_t victim = uint8_t(next(8));
		const uint32_t k = next(10);
		const uint8_t killer = k == 9 ? uint8_t(255) : k == 8 ? victim : uint8_t(next(8));
		log.push_back({ killer, victim, uint16_t(next(6)), next(3) == 0 });
	}
	for (int i = 0; i < 6; ++i) {
		log.push_back({ 2, 1, 5, i == 3 });
	}

	std::array<std::array<uint32_t, 8>, 8> kills{};
	std::array<std::array<uint32_t, 8>, 8> run{};
	std::array<uint32_t, 6> by_def{};
	uint32_t own_kills = 0, own_deaths = 0, streak = 0, longest = 0, nemesis = 0, dogtags = 0, unknown_killers = 0;
	float headshot = 0.0f;
	const auto team = [](uint8_t p_slot) { return 1 + p_slot % 2; };
	bf3::sim::Tick tick = 700;
	for (const Kill &k : log) {
		const bool suicide = k.killer == k.victim || k.killer >= 8;
		const bool tk = !suicide && team(k.killer) == team(k.victim);
		unknown_killers += k.killer >= 8 ? 1 : 0;
		if (!suicide && !tk) {
			++kills[k.killer][k.victim];
			++run[k.killer][k.victim];
			run[k.victim][k.killer] = 0;
		}
		if (k.victim == 2) {
			++own_deaths;
			streak = 0;
		} else if (k.killer == 2 && !suicide && !tk) {
			++own_kills;
			++by_def[k.def];
			dogtags += k.def == 5 ? 1 : 0;
			longest = std::max(longest, ++streak);
			nemesis = std::max(nemesis, run[2][k.victim]);
			if (k.headshot) {
				headshot = std::max(headshot,
						std::sqrt(
								float(k.victim * 3) * float(k.victim * 3) + float(k.victim * 4) * float(k.victim * 4)));
			}
		}
		uint8_t flags = k.headshot ? host::present_flags::kHeadshot : uint8_t(0);
		flags |= k.killer == 2 ? host::present_flags::kOwnShooter : uint8_t(0);
		flags |= k.victim == 2 ? host::present_flags::kOwnVictim : uint8_t(0);
		const host::PresentEvent e = death(k.killer, k.victim, k.def, flags, tick);
		in.server_tick = ++tick;
		in.now_s = double(tick) / 60.0;
		host::fill_hud_tdm(in, std::span<const host::PresentEvent>(&e, 1), st, state, m);
		if (k.victim == 2 && !suicide) {
			CHECK(m.kill_card.killer_player_kills == int32_t(kills[k.killer][2]));
			CHECK(m.kill_card.player_killer_kills == int32_t(kills[2][k.killer]));
			CHECK(name_of(m.kill_card.killer_name) == "Bot");
			CHECK(m.kill_card.killer_score == int32_t(k.killer) * 10);
		}
	}
	uint32_t mismatched = 0;
	for (uint8_t a = 0; a < 8; ++a) {
		for (uint8_t b = 0; b < 8; ++b) {
			mismatched += host::round_stats_kills(state.stats, a, b) != kills[a][b] ? 1 : 0;
			mismatched += host::round_stats_nemesis(state.stats, a, b) != run[a][b] ? 1 : 0;
		}
	}
	CHECK(mismatched == 0);
	CHECK(state.stats.own_kills == own_kills);
	CHECK(state.stats.own_deaths == own_deaths);
	CHECK(state.stats.longest_streak == longest);
	CHECK(state.stats.longest_nemesis_streak == nemesis);
	CHECK(state.stats.longest_headshot_m == doctest::Approx(headshot));
	CHECK(state.stats.deaths_seen == log.size());
	CHECK(own_kills >= 6);
	const uint16_t best_def = uint16_t(std::max_element(by_def.begin(), by_def.end()) - by_def.begin());

	in.match.phase = net::MatchPhase::RoundOver;
	in.match.round_end_tick = 600 + 60 * 300;
	in.match.winner = 1;
	in.match.kills = { 100, 87 };
	in.server_tick = in.match.round_end_tick + 30;
	host::fill_hud_tdm(in, {}, st, state, m);
	const ui::HudEndOfRound &e = m.end_of_round;
	CHECK(e.visible);
	CHECK(e.kills == int32_t(own_kills));
	CHECK(e.deaths == int32_t(own_deaths));
	CHECK(e.kill_death_ratio == doctest::Approx(double(own_kills) / double(own_deaths)));
	CHECK(e.best_weapon_info.item_id == 1000u + best_def);
	CHECK(e.best_weapon_info.kills == int32_t(by_def[best_def]));
	CHECK(e.dogtags_taken == int32_t(dogtags));
	CHECK(e.round_duration == doctest::Approx(300.0f));
	CHECK(e.kills_per_minute == doctest::Approx(double(own_kills) / 5.0));
	CHECK(e.score_per_minute == doctest::Approx(1500.0 / 5.0));
	CHECK(e.total_score == 1500);
	CHECK(e.longest_streak == int32_t(longest));
	CHECK(e.longest_nemesis_streak == int32_t(nemesis));
	CHECK(e.longest_headshot == doctest::Approx(headshot));
	CHECK(name_of(e.name) == "Me");
	CHECK(e.winning_team == 1);
	CHECK(e.local_player_in_winning_team);
	CHECK(e.team_results == std::array<int32_t, 2>{ 100, 87 });
	CHECK(e.player_team_index == 1);
	CHECK(e.in_match);
	CHECK(!e.is_ranked);
	CHECK(e.stage == ui::HudEorStage::None);

	bf3::core::gameplay::EorTimeline timeline;
	timeline.fade_s = 1.0f;
	timeline.scoreboard_delay_s = 4.0f;
	timeline.request_eor_delay_s = 9.0f;
	timeline.pre_eor_s = 12.0f;
	timeline.eor_s = 48.0f;
	timeline.restart_delay_s = 60.0f;
	in.eor = &timeline;
	in.match.unspawn_tick = in.match.round_end_tick + 60;
	in.match.restart_tick = in.match.unspawn_tick + 60 * 60;
	const std::array<std::pair<double, ui::HudEorStage>, 9> stages{ { { 0.0, ui::HudEorStage::Fade },
			{ 0.5, ui::HudEorStage::Fade }, { 1.0, ui::HudEorStage::PreEor }, { 4.5, ui::HudEorStage::PreEor },
			{ 5.0, ui::HudEorStage::Scoreboard }, { 11.5, ui::HudEorStage::Scoreboard }, { 12.0, ui::HudEorStage::Eor },
			{ 30.0, ui::HudEorStage::Eor }, { 59.5, ui::HudEorStage::Eor } } };
	const std::array<double, 4> starts{ 0.0, 1.0, 5.0, 12.0 };
	size_t stage_misses = 0;
	for (const auto &[at, stage] : stages) {
		in.server_tick = in.match.unspawn_tick + bf3::sim::Tick(at * 60.0);
		host::fill_hud_tdm(in, {}, st, state, m);
		const double since = at - starts[size_t(stage) - 1];
		const bool hit = m.end_of_round.stage == stage && std::abs(m.end_of_round.stage_seconds - since) < 1e-4 &&
				std::abs(m.end_of_round.time_until_level_change - (60.0 - at)) < 1e-4;
		stage_misses += hit ? 0 : 1;
	}
	CHECK(stage_misses == 0);
	in.server_tick = in.match.unspawn_tick - 1;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(m.end_of_round.stage == ui::HudEorStage::None);
	in.eor = nullptr;

	in.match.phase = net::MatchPhase::PreRound;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(state.stats.own_kills == 0);
	CHECK(host::round_stats_kills(state.stats, 2, 1) == 0);
	CHECK(state.bad_inputs == unknown_killers);
	row("unit.round_stats", mismatched == 0 && state.stats.own_kills == 0 && stage_misses == 0,
			std::to_string(log.size()) + " deaths replayed, " + std::to_string(own_kills) + " own kills, " +
					std::to_string(own_deaths) + " own deaths, streak " + std::to_string(longest) + ", nemesis " +
					std::to_string(nemesis) + ", best def " + std::to_string(best_def));
}

TEST_CASE("own kills raise the scoring table's kill and headshot score messages") {
	const host::HudStatic st = make_static();
	std::vector<net::PlayerRow> rows{ player(1, 2, "Bot RU 1", 300, 3), player(2, 1, "Bot US 1", 200, 2), player(3, 1, "Me", 0, 0, false) };
	host::HudFeedInput in;
	in.tdm = true;
	in.match_valid = true;
	in.match.phase = net::MatchPhase::InRound;
	in.players = rows;
	in.own_slot = 3;
	in.own_team = 1;
	in.alive = true;
	in.score_kill = 100;
	in.score_headshot = 10;
	host::HudTdmFeedState state;
	ui::HudModel m;
	const std::array<host::PresentEvent, 4> events{ death(3, 1, 0, host::present_flags::kOwnShooter), death(3, 1, 0, host::present_flags::kOwnShooter | host::present_flags::kHeadshot),
		death(3, 2, 0, host::present_flags::kOwnShooter), death(2, 1, 0) };
	host::fill_hud_tdm(in, events, st, state, m);
	const ui::HudScoreFeed &f = m.messages.score_message;
	REQUIRE(f.newest_serial == 3);
	CHECK(f.events[1].sid_hash == ui::text_hash("ID_SCORE_KILL"));
	CHECK(f.events[1].points == 100);
	CHECK(f.events[2].sid_hash == ui::text_hash("ID_SCORE_KILL"));
	CHECK(f.events[3].sid_hash == ui::text_hash("ID_SCORE_HEADSHOT"));
	CHECK(f.events[3].points == 10);
	CHECK(m.messages.local_kill_message.newest_serial == 3);
	CHECK(m.messages.local_kill_message.lines[3].team_kill);
}

TEST_CASE("the kit bar: the own team's loadouts in slots 0, 1, 2, 6, 7") {
	const host::HudStatic st = make_static();
	host::KitTable kits;
	for (uint8_t team = 0; team < host::kKitTeams; ++team) {
		for (uint8_t k = 0; k < host::kKitCount; ++k) {
			host::KitLoadout &l = kits.loadouts[team][k];
			l.kit = k;
			l.team = uint8_t(team + 1);
			l.defs.fill(sw::kNoWeapon);
			l.defs[0] = uint16_t(team == 0 ? 0 : 1);
			l.defs[1] = 2;
			l.defs[6] = 3;
			l.defs[7] = 4;
			l.hud_icon = int32_t(70 + 2 * k + team);
		}
		kits.loadouts[team][0].defs[host::kKitGadgetSlot] = 5;
	}
	kits.loadouts[1][3].defs[1] = 6;
	host::HudFeedInput in;
	in.tdm = true;
	in.match_valid = true;
	in.match.phase = net::MatchPhase::InRound;
	in.own_slot = 1;
	in.own_team = 2;
	in.alive = false;
	in.kits = &kits;
	host::HudTdmFeedState state;
	ui::HudModel m;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(m.spawn_screen.visible);
	CHECK(m.spawn_screen.kit_items[0] == std::array<uint32_t, 5>{ 1001, 1002, 1005, 1003, 1004 });
	CHECK(m.spawn_screen.kit_items[1] == std::array<uint32_t, 5>{ 1001, 1002, 0, 1003, 1004 });
	CHECK(m.spawn_screen.kit_items[3] == std::array<uint32_t, 5>{ 1001, 0, 0, 1003, 1004 });
	std::vector<net::PlayerRow> rows{ player(1, 2, "Own"), player(4, 1, "Other"), player(6, 0, "Unassigned") };
	rows[0].kit = 3;
	rows[1].kit = 1;
	rows[2].kit = 2;
	in.players = rows;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(m.players[1].kit_icon == 77);
	CHECK(m.players[4].kit_icon == 72);
	CHECK(m.players[6].kit_icon == 0);
	in.own_team = 0;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(m.spawn_screen.kit_items[0][0] == 1000);
	CHECK(state.bad_inputs == 0);
}

TEST_CASE("the deploy list: one row per enabled spawner of the own team, labelled by its LocationTextSid") {
	const host::HudStatic st = make_static();
	std::vector<bf3::core::level::CharacterSpawnerDef> spawners(4);
	spawners[0].team = 2;
	spawners[0].location_text_sid = "ID_H_MP_DM_RANDOMSPAWN";
	spawners[1].team = 1;
	spawners[1].location_text_sid = "ID_H_MP_DM_RANDOMSPAWN";
	spawners[2].team = 2;
	spawners[2].enabled = false;
	spawners[2].location_text_sid = "ID_DISABLED";
	spawners[3].team = 2;
	host::HudFeedInput in;
	in.tdm = true;
	in.match_valid = true;
	in.match.phase = net::MatchPhase::InRound;
	in.own_slot = 1;
	in.own_team = 2;
	in.alive = false;
	in.spawners = spawners;
	host::HudTdmFeedState state;
	ui::HudModel m;
	host::fill_hud_tdm(in, {}, st, state, m);
	const ui::HudSpawnPointList &list = m.spawn_points.spawn_points;
	REQUIRE(list.count == 1);
	CHECK(list.points[0].kind == ui::HudSpawnKind::Hq);
	CHECK(list.points[0].team == 2);
	CHECK(list.points[0].available);
	CHECK(list.points[0].label_sid_hash == ui::text_hash("ID_H_MP_DM_RANDOMSPAWN"));
	CHECK(list.points[0].player == ui::kHudNoPlayer);
	CHECK(m.spawn_points.has_valid_spawn);
	in.match_valid = false;
	host::fill_hud_tdm(in, {}, st, state, m);
	REQUIRE(list.count == 1);
	CHECK_FALSE(list.points[0].available);
	in.match_valid = true;
	in.own_team = 0;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(list.count == 0);
	std::vector<bf3::core::level::CharacterSpawnerDef> many(ui::kHudMaxSpawnPoints + 3, spawners[0]);
	in.own_team = 2;
	in.spawners = many;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(list.count == ui::kHudMaxSpawnPoints);
	CHECK(state.rows_dropped == 3);
	in.spawners = {};
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(list.count == 0);
	CHECK(state.bad_inputs == 0);
}

TEST_CASE("the loadout screens: kit and customization from the catalog and the own choices; the menu, user, settings "
		  "and level") {
	const host::HudStatic st = make_static();
	host::KitTable kits;
	for (uint8_t team = 0; team < host::kKitTeams; ++team) {
		for (uint8_t k = 0; k < host::kKitCount; ++k) {
			host::KitLoadout &l = kits.loadouts[team][k];
			l.kit = k;
			l.team = uint8_t(team + 1);
			l.defs.fill(sw::kNoWeapon);
			l.defs[0] = 0;
			l.defs[1] = 2;
			l.defs[6] = 3;
			l.defs[7] = 4;
			l.hud_icon = int32_t(70 + 2 * k + team);
		}
		kits.loadouts[team][0].defs[host::kKitGadgetSlot] = 5;
	}
	host::LoadoutCatalog catalog;
	host::LoadoutKit &assault = catalog.kits[0][0];
	const auto item = [](uint32_t p_id, uint16_t p_def, std::vector<uint32_t> p_optics = {}) {
		host::LoadoutItem i;
		i.identifier = p_id;
		i.unlock = "u_" + std::to_string(p_id);
		i.def = p_def;
		i.accessories[0] = std::move(p_optics);
		return i;
	};
	assault.slots[0] = { item(1000, 0, { 201, 202 }), item(1001, 1, { 203 }) };
	assault.slots[1] = { item(1002, 2) };
	assault.slots[2] = { item(401, sw::kNoWeapon), item(402, sw::kNoWeapon) };
	assault.slots[5] = { item(501, sw::kNoWeapon) };
	assault.slots[6] = { item(1003, 3) };
	assault.slots[7] = { item(1004, 4) };
	assault.defaults = { 1000, 1002, 1005, 0, 0, 0, 1003, 1004 };
	assault.specializations = { 301, 302 };
	assault.label_sid_hash = ui::text_hash("ID_M_ASSAULT");
	catalog.items = 8;
	std::vector<net::PlayerRow> rows{ player(1, 1, "Own"), player(4, 2, "Other") };
	host::HudFeedInput in;
	in.tdm = true;
	in.match_valid = true;
	in.match.phase = net::MatchPhase::InRound;
	in.players = rows;
	in.own_slot = 1;
	in.own_team = 1;
	in.alive = false;
	in.kits = &kits;
	in.loadouts = &catalog;
	in.game_mode = "TeamDeathMatch0";
	in.level_name_sid_hash = 0x90111346u;
	host::HudTdmFeedState state;
	ui::HudModel m;
	host::fill_hud_tdm(in, {}, st, state, m);
	const ui::HudCustomization &c = m.customization;
	CHECK(m.spawn_screen.kit_items[0] == std::array<uint32_t, 5>{ 1000, 1002, 1005, 1003, 1004 });
	CHECK(m.kit.selected_kit == 0);
	CHECK(m.kit.number_of_kits == 4);
	CHECK(m.kit.selected_kit_name == ui::text_hash("ID_M_ASSAULT"));
	CHECK(c.customization_team == 1);
	CHECK(c.kits_data[0].name_sid_hash == ui::text_hash("ID_M_ASSAULT"));
	CHECK(c.kits_data[0].hud_icon == 70);
	CHECK(c.kits_data[2].hud_icon == 74);
	CHECK(c.kits_data[0].items[0].item_id == 1000);
	CHECK(c.kits_data[0].items[2].item_id == 1005);
	CHECK(c.kit_primary_weapon.item_id == 1000);
	CHECK(c.kit_secondary_weapon.item_id == 1002);
	CHECK(c.kit_gadget1.item_id == 1005);
	CHECK(c.kit_gadget2.item_id == 0);
	CHECK(c.kit_specialization.item_id == 0);
	CHECK(c.weapon_accessory_main.item_id == 1000);
	CHECK(c.kit_grid_data[0].count == 2);
	CHECK(c.kit_grid_data[0].items[1].item_id == 1001);
	CHECK(c.kit_grid_selected[0] == 0);
	CHECK(c.kit_grid_data[2].count == 2);
	CHECK(c.kit_grid_selected[2] == -1);
	CHECK(c.kit_grid_data[4].count == 1);
	CHECK(c.kit_grid_selected[4] == 0);
	CHECK(c.accessory_grid[0].count == 2);
	CHECK(c.accessory_grid[0].items[1].item_id == 202);
	CHECK(c.accessory_selected[0] == -1);
	CHECK(c.accessory_grid[1].count == 0);
	CHECK(state.bad_inputs == 0);

	host::LoadoutChoice pick;
	pick.items[0] = 1001;
	pick.items[2] = 402;
	pick.items[5] = 501;
	pick.accessories[0] = 203;
	pick.specialization = 302;
	in.loadout_choices[0] = pick;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(m.spawn_screen.kit_items[0] == std::array<uint32_t, 5>{ 1001, 1002, 1005, 1003, 1004 });
	CHECK(c.kits_data[0].items[0].item_id == 1001);
	CHECK(c.kits_data[0].items[2].item_id == 402);
	CHECK(c.kit_primary_weapon.item_id == 1001);
	CHECK(c.kit_gadget1.item_id == 402);
	CHECK(c.kit_gadget2.item_id == 501);
	CHECK(c.kit_specialization.item_id == 302);
	CHECK(c.weapon_accessory_main.item_id == 1001);
	CHECK(c.weapon_accessory1.item_id == 203);
	CHECK(c.weapon_accessory2.item_id == 0);
	CHECK(c.kit_grid_selected[0] == 1);
	CHECK(c.kit_grid_selected[2] == 1);
	CHECK(c.accessory_grid[0].count == 1);
	CHECK(c.accessory_selected[0] == 0);
	CHECK(state.bad_inputs == 0);
	in.loadout_choices[0].items[0] = 999;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(state.bad_inputs == 1);
	CHECK(c.kit_primary_weapon.item_id == 1000);
	CHECK(m.spawn_screen.kit_items[0][0] == 1000);
	in.loadout_choices[0] = pick;

	const ui::HudMenuList &menu = m.ingame_menu.menu_entries;
	REQUIRE(menu.count == host::kHudMenuSids.size());
	for (size_t i = 0; i < menu.count; ++i) {
		CHECK(menu.entries[i].label_sid_hash == ui::text_hash(host::kHudMenuSids[i]));
		CHECK(menu.entries[i].action == ui::HudMenuAction(i));
	}
	CHECK(menu.entries[0].enabled);
	CHECK(menu.entries[1].enabled);
	CHECK_FALSE(menu.entries[2].enabled);
	CHECK_FALSE(menu.entries[3].enabled);
	CHECK(menu.entries[4].enabled);
	in.alive = true;
	in.in_menu = true;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(menu.entries[2].enabled);
	CHECK(m.user.is_in_menu);
	CHECK(m.user.is_ingame);
	CHECK(std::string(m.user.user_name.data()) == "Own");
	CHECK(std::string(m.settings.current_game_mode.data()) == "TeamDeathMatch0");
	CHECK(m.settings.current_game_mode_name == ui::text_hash(host::kHudTdmModeSid));
	CHECK(m.settings.is_pc);
	CHECK(m.level.current_level == 0x90111346u);
	in.match.phase = net::MatchPhase::RoundOver;
	in.in_menu = false;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK_FALSE(menu.entries[2].enabled);
	CHECK_FALSE(m.user.is_in_menu);
	in.match_valid = false;
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK_FALSE(m.user.is_ingame);
	CHECK_FALSE(menu.entries[2].enabled);
	CHECK(state.bad_inputs == 1);
	CHECK(state.rows_dropped == 0);
	in.match_valid = true;
	for (uint32_t id = 600; assault.slots[2].size() < ui::kHudGridItems + 2; ++id) {
		assault.slots[2].push_back(item(id, sw::kNoWeapon));
	}
	host::fill_hud_tdm(in, {}, st, state, m);
	CHECK(c.kit_grid_data[2].count == ui::kHudGridItems);
	CHECK(c.kit_grid_selected[2] == 1);
	CHECK(state.rows_dropped == 2);
	row("unit.loadout_screens", state.bad_inputs == 1 && c.kit_primary_weapon.item_id == 1001,
			"kit, customization grids and accessories from a 8-item catalog, a bad stored choice falls back to the "
			"defaults, 5 menu entries");
}

TEST_CASE("hostile inputs are skipped or clamped and counted") {
	const host::HudStatic st = make_static();
	std::vector<net::PlayerRow> rows{ player(200, 1, "Spectator slot"), player(3, 9, "Bad team"), player(4, 1, "Bad kit") };
	rows[2].kit = 77;
	rows[0].name_bytes = 200;
	net::PlayerRow long_name = player(5, 1, "SixteenCharsName");
	long_name.name_bytes = 250;
	rows.push_back(long_name);
	host::HudFeedInput in;
	in.tdm = true;
	in.match_valid = true;
	in.match.phase = net::MatchPhase(7);
	in.match.winner = 9;
	in.match.kills = { -5, 3 };
	in.match.kill_limit = -1;
	in.players = rows;
	in.own_slot = 70;
	in.own_team = 5;
	in.spawn_kit = 9;
	in.alive = false;
	in.now_s = std::numeric_limits<double>::quiet_NaN();
	in.server_tick = 10;
	in.own_respawn_tick = std::numeric_limits<uint32_t>::max();
	in.own_revive_until = std::numeric_limits<uint32_t>::max();
	in.kill_card_source.killer_seen = true;
	in.kill_card_source.distance_m = std::numeric_limits<float>::infinity();
	std::vector<host::PresentEvent> events{ death(1, 99, 0), death(250, 3, 0, host::present_flags::kOwnVictim), death(1, 2, 0xFFFE) };
	events[1].value = std::numeric_limits<float>::quiet_NaN();
	host::HudTdmFeedState state;
	ui::HudModel m;
	host::fill_hud_tdm(in, events, st, state, m);
	CHECK(state.bad_inputs >= 10);
	CHECK(m.player_count == 6);
	CHECK(m.players[3].team == 0);
	CHECK(m.players[4].kit == 0);
	CHECK(name_of(m.players[5].name) == "SixteenCharsName");
	CHECK(m.local_player == ui::kHudNoPlayer);
	CHECK(m.local_team == 0);
	CHECK(m.kill_counter.team1_kills == 0);
	CHECK(m.kill_counter.team1_max_kills == 0);
	CHECK(m.messages.kill_message.newest_serial == 2);
	CHECK(state.unknown_defs == 1);
	CHECK(m.kill_card.serial == 1);
	CHECK(m.kill_card.killer == ui::kHudNoPlayer);
	CHECK(m.kill_card.killer_health == 0.0f);
	CHECK(m.kill_card.distance_m < 0.0f);
	CHECK(!m.end_of_round.visible);
	CHECK(!m.pre_round.visible);
	CHECK(m.end_of_round.winner == 0);
	CHECK(m.spawn_screen.selected_kit == 0);
	CHECK(!m.spawn_screen.can_deploy);
	CHECK(std::isfinite(m.spawn_screen.seconds_to_deploy));
	CHECK(m.spawn_screen.seconds_to_deploy <= 3600.0f);
	CHECK(m.spawn_screen.seconds_revive_left <= 3600.0f);
	CHECK(std::isfinite(m.kill_card.seconds_left));

	const host::PresentEvent odd = spawn_event(255);
	host::fill_hud_tdm(in, std::span<const host::PresentEvent>(&odd, 1), st, state, m);
	CHECK(state.frames == 2);
}

TEST_CASE("m2b off: every M2b screen hidden and the M2a part of the model untouched") {
	const host::HudStatic st = make_static();
	ui::HudModel m;
	m.serial = 41;
	m.weapon.active_weapon_ammo = 17;
	m.soldier.health = 55.0f;
	m.kill_card.visible = true;
	m.spawn_screen.visible = true;
	m.pre_round.visible = true;
	m.end_of_round.visible = true;
	std::vector<uint8_t> before(offsetof(ui::HudModel, kill_card));
	std::memcpy(before.data(), &m, before.size());
	host::HudFeedInput in;
	in.match_valid = true;
	in.match.phase = net::MatchPhase::RoundOver;
	const host::PresentEvent d = death(1, 2, 0);
	host::HudTdmFeedState state;
	host::fill_hud_tdm(in, std::span<const host::PresentEvent>(&d, 1), st, state, m);
	CHECK(!m.kill_card.visible);
	CHECK(!m.spawn_screen.visible);
	CHECK(!m.pre_round.visible);
	CHECK(!m.end_of_round.visible);
	CHECK(std::memcmp(&m, before.data(), before.size()) == 0);
	CHECK(state.deaths_fed == 0);
}

TEST_CASE("the lookup lists: SIDs, fonts, icons and placements are well formed") {
	std::set<std::string_view> sids;
	for (size_t i = 0; i < gs::kTdmTexts.size(); ++i) {
		const gs::TdmTextEntry &e = gs::kTdmTexts[i];
		CHECK(size_t(e.key) == i);
		CHECK(e.sid.starts_with("ID_"));
		CHECK(std::all_of(e.sid.begin(), e.sid.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }));
		CHECK(!e.english.empty());
		CHECK(sids.insert(e.sid).second);
	}
	CHECK(gs::tdm_text_sid(gs::TdmText::Deploy) == "ID_M_SPAWN_DEPLOY");
	CHECK(gs::tdm_text_sid(gs::TdmText::Count).empty());
	for (std::string_view f : gs::kTdmFonts) {
		CHECK(f.starts_with("$"));
	}
	for (std::string_view r : gs::kTdmKitIcons) {
		CHECK(r.starts_with("mc_iconsIngameSmall/Kit"));
	}
	for (const gs::TdmPlacement &p : gs::kTdmPlacements) {
		CHECK(p.movie.starts_with("ui/assets/"));
		CHECK(!p.instance.empty());
	}
}

}

namespace {

struct TdmInstall {
	bf3::core::InstallReader reader;
	bf3::core::InstallSnapshot before;
	std::shared_ptr<bf3::core::Install> install;
};

TdmInstall *tdm_install() {
	static TdmInstall *s = [] {
		TdmInstall *h = new TdmInstall();
		if (!h->reader.locate({})) {
			return h;
		}
		h->before = h->reader.snapshot();
		bf3::core::Result<std::shared_ptr<bf3::core::Install>> opened = bf3::core::Install::open(h->reader);
		if (opened) {
			h->install = *opened;
		}
		return h;
	}();
	if (s->install == nullptr) {
		bf3::test::skip_install_case(s->reader.describe().c_str());
		return nullptr;
	}
	return s;
}

struct KitItem {
	std::string_view variant;
	std::string_view name;
};
constexpr KitItem kTdmKitItems[] = {
	{ "weapons/m16a4/u_m16a4", "M16A3" },
	{ "weapons/m4a1/u_m4a1", "M4A1" },
	{ "weapons/m27iar/u_m27iar", "M27 IAR" },
	{ "weapons/mk11/u_mk11", "MK11 MOD 0" },
	{ "weapons/ak74m/u_ak74m", "AK-74M" },
	{ "weapons/aks74u/u_aks74u", "AKS-74u" },
	{ "weapons/rpk/u_rpk-74m", "RPK-74M" },
	{ "weapons/svd/u_svd", "SVD" },
	{ "weapons/m9/u_m9", "M9" },
	{ "weapons/mp443/u_mp443", "MP443" },
	{ "weapons/knife/u_knife", "KNIFE" },
	{ "weapons/m67/u_m67", "M67 GRENADE" },
	{ host::kTdmAssaultGadget, "DEFIBRILLATOR" },
};

}

TEST_SUITE("ui-hud-tdm-install") {

TEST_CASE("every string resolves to its recorded English") {
	TdmInstall *ti = tdm_install();
	if (ti == nullptr) {
		return;
	}
	bf3::core::Result<ui::TextDatabase> text = ui::open_text_database(ti->install);
	REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
	size_t ok = 0;
	constexpr gs::TdmTextEntry kFeedTexts[] = {
		{ gs::TdmText::Count, host::kHudMenuSids[0], "RESUME" },
		{ gs::TdmText::Count, host::kHudMenuSids[1], "SCOREBOARD" },
		{ gs::TdmText::Count, host::kHudMenuSids[2], "SUICIDE" },
		{ gs::TdmText::Count, host::kHudMenuSids[3], "OPTIONS" },
		{ gs::TdmText::Count, host::kHudMenuSids[4], "QUIT" },
		{ gs::TdmText::Count, host::kHudTdmModeSid, "TEAM DEATHMATCH" },
		{ gs::TdmText::Count, "ID_H_MP_DM_RANDOMSPAWN", "RANDOM SPAWN POINT" },
		{ gs::TdmText::Count, "ID_M_LEVELNAME_MP_SUBWAY", "OPERATION M\xC3\x89TRO" },
	};
	for (const gs::TdmTextEntry &e : kFeedTexts) {
		const std::optional<std::string_view> t = text->find_sid(e.sid);
		const bool pass = t.has_value() && *t == e.english && text->resolves(e.sid);
		row(std::string("text.") + std::string(e.sid), pass,
				"'" + (t ? std::string(*t) : std::string("absent")) + "' (want '" + std::string(e.english) + "')");
		CHECK_MESSAGE(pass, std::string(e.sid));
	}
	for (const gs::TdmTextEntry &e : gs::kTdmTexts) {
		const std::optional<std::string_view> t = text->find_sid(e.sid);
		const bool pass = t.has_value() && *t == e.english && text->resolves(e.sid);
		ok += pass ? 1 : 0;
		row(std::string("text.") + std::string(e.sid), pass, "'" + (t ? std::string(*t) : std::string("absent")) + "' (want '" + std::string(e.english) + "')");
		CHECK_MESSAGE(pass, std::string(e.sid));
	}
	row("text.all", ok == gs::kTdmTexts.size(), std::to_string(ok) + "/" + std::to_string(gs::kTdmTexts.size()) + " strings");
}

TEST_CASE("every font resolves to a BF3 font of the en mapping") {
	TdmInstall *ti = tdm_install();
	if (ti == nullptr) {
		return;
	}
	bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> ebx = bf3::core::EbxLibrary::open(ti->install, { "Win32/UI" });
	REQUIRE_MESSAGE(ebx.ok(), (ebx.ok() ? std::string() : ebx.error().message));
	bf3::core::Result<ui::FontMappingCollection> mapping = ui::font_mapping_for_text_database(**ebx, "localization/us_loc");
	REQUIRE_MESSAGE(mapping.ok(), (mapping.ok() ? std::string() : mapping.error().message));
	for (std::string_view f : gs::kTdmFonts) {
		const ui::ResolvedFont r = ui::resolve_font_name(*mapping, f);
		const bool pass = (r.how == ui::FontResolution::Exact || r.how == ui::FontResolution::CaseFolded || r.how == ui::FontResolution::Family) &&
				!r.font_long_name.empty();
		row(std::string("font.") + std::string(f), pass, std::string(ui::font_resolution_name(r.how)) + " -> '" + r.font_long_name + "' via " + r.via);
		CHECK_MESSAGE(pass, std::string(f));
	}
}

TEST_CASE("every atlas icon and placement resolves") {
	TdmInstall *ti = tdm_install();
	if (ti == nullptr) {
		return;
	}
	bf3::core::Result<ui::SwfLibrary> lib = ui::SwfLibrary::open(ti->install);
	REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
	bf3::core::Result<std::shared_ptr<const ui::SwfMovie>> movie = lib->load(gs::kTdmIconMovie);
	REQUIRE_MESSAGE(movie.ok(), (movie.ok() ? std::string() : movie.error().message));
	bf3::core::level::WorkBudget budget(ui::kSwfAtlasBudgetUnits);
	bf3::core::Result<ui::SwfAtlasMap> map = ui::build_atlas_map(**movie, gs::kTdmIconMovie, budget);
	REQUIRE_MESSAGE(map.ok(), (map.ok() ? std::string() : map.error().message));
	std::vector<std::string_view> regions(gs::kTdmKitIcons.begin(), gs::kTdmKitIcons.end());
	regions.push_back(gs::kTdmDeadIcon);
	for (std::string_view name : regions) {
		const ui::SwfAtlasRegion *r = map->find(name);
		const bool pass = r != nullptr && !r->pieces.empty();
		row(std::string("icon.") + std::string(name), pass, r != nullptr ? std::to_string(r->pieces.size()) + " piece(s)" : std::string("no region"));
		CHECK_MESSAGE(pass, std::string(name));
	}
	for (const gs::TdmPlacement &p : gs::kTdmPlacements) {
		bf3::core::level::WorkBudget pb(ui::kSwfLayoutBudgetUnits);
		bf3::core::Result<std::vector<ui::HudPlacement>> rows = ui::read_hud_placements(*lib, p.movie, pb);
		REQUIRE_MESSAGE(rows.ok(), (rows.ok() ? std::string() : rows.error().message));
		const ui::HudPlacement *found = nullptr;
		for (const ui::HudPlacement &h : *rows) {
			if (h.instance == p.instance) {
				found = &h;
			}
		}
		const bool pass = found != nullptr && std::abs(found->matrix.tx - p.x) < 0.5f && std::abs(found->matrix.ty - p.y) < 0.5f && found->anchor == p.anchor;
		char detail[160];
		std::snprintf(detail, sizeof(detail), "at (%.2f, %.2f) %s (fallback %.0f, %.0f %s)", found ? found->matrix.tx : -1.0f, found ? found->matrix.ty : -1.0f,
				found ? ui::hud_anchor_name(found->anchor) : "-", p.x, p.y, ui::hud_anchor_name(p.anchor));
		row("placement." + std::string(p.movie) + "/" + std::string(p.instance), pass, detail);
		CHECK_MESSAGE(pass, std::string(p.instance));
	}
}

TEST_CASE("the 13 TDM kit items: names and _small icons") {
	TdmInstall *ti = tdm_install();
	if (ti == nullptr) {
		return;
	}
	bf3::core::Result<sw::InstallWeapons> weapons = sw::open_install_weapons(ti->install);
	REQUIRE_MESSAGE(weapons.ok(), (weapons.ok() ? std::string() : weapons.error().message));
	bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> ebx = bf3::core::EbxLibrary::open(ti->install, { "Win32/UI" });
	REQUIRE_MESSAGE(ebx.ok(), (ebx.ok() ? std::string() : ebx.error().message));
	bf3::core::Result<ui::ItemIconIndex> items = ui::ItemIconIndex::read(**ebx);
	REQUIRE_MESSAGE(items.ok(), (items.ok() ? std::string() : items.error().message));
	bf3::core::Result<ui::TextDatabase> text = ui::open_text_database(ti->install);
	REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
	host::HudStatic table;
	const size_t table_items = host::apply_item_table(table, *items);
	CHECK(table_items > std::size(kTdmKitItems));
	size_t ok = 0;
	size_t pathed = 0;
	for (const KitItem &k : kTdmKitItems) {
		bf3::core::Result<gp::WeaponDef> d = weapons->catalog->resolve(std::string(k.variant));
		REQUIRE_MESSAGE(d.ok(), (std::string(k.variant) + ": " + (d.ok() ? std::string() : d.error().message)));
		const ui::ItemIcon *icon = items->find(d->unlock_identifier);

		uint32_t name_hash = d->name_sid_hash;
		if (name_hash == 0 && icon != nullptr && !icon->name_sid.empty()) {
			name_hash = ui::text_hash(icon->name_sid);
		}
		const std::optional<std::string_view> name = text->find(name_hash);
		const bool name_ok = name.has_value() && !name->empty() && *name != "<MISSING>" && (k.name.empty() || *name == k.name);
		bool icon_ok = icon != nullptr && icon->res_name.ends_with("_small");
		std::string icon_detail = icon != nullptr ? icon->res_name : std::string("no description");
		if (icon_ok) {
			bf3::core::Result<ui::ItemIconImage> img = ui::load_item_icon((*ebx)->mount(0), *icon);
			icon_ok = img.ok() && img->width > 0 && img->height > 0 && !img->data.empty();
			icon_detail += img.ok() ? " " + std::to_string(img->width) + "x" + std::to_string(img->height) : ": " + img.error().message;
		}
		const ui::HudItem item = host::hud_item(table, d->unlock_identifier);
		const bool path_ok = icon != nullptr && !icon->path.empty() &&
				std::string_view(item.image_path.data()) == icon->path.substr(0, item.image_path.size() - 1);
		pathed += path_ok ? 1 : 0;
		CHECK_MESSAGE(path_ok, std::string(k.variant));
		ok += name_ok && icon_ok ? 1 : 0;
		row("kit." + std::string(k.variant), name_ok && icon_ok,
				"item " + std::to_string(d->unlock_identifier) + ", name '" + (name ? std::string(*name) : std::string("?")) + "', icon " + icon_detail);
		CHECK_MESSAGE(name_ok, std::string(k.variant));
		CHECK_MESSAGE(icon_ok, std::string(k.variant));
	}
	row("kit.all", ok == std::size(kTdmKitItems), std::to_string(ok) + "/" + std::to_string(std::size(kTdmKitItems)) + " kit items with a name and an icon");
	row("kit.item_table", pathed == std::size(kTdmKitItems),
			std::to_string(table_items) + " items in the HUD item table, " + std::to_string(pathed) + "/" +
					std::to_string(std::size(kTdmKitItems)) + " kit items with their image path");
}

TEST_CASE("install.unchanged") {
	TdmInstall *ti = tdm_install();
	if (ti == nullptr) {
		return;
	}
	const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(ti->before, ti->reader.snapshot());
	row("install.unchanged", ti->before.valid && d.clean(), std::to_string(ti->before.files.size()) + " index files" + (d.clean() ? "" : ": " + d.summary()));
	CHECK(ti->before.valid);
	CHECK(d.clean());
}

}
