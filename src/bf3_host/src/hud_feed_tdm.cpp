#include "bf3/host/hud_feed.h"

#include "bf3/host/kits.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/sim/weapons/sim_time.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace bf3::host {

namespace ui = bf3::core::ui;
namespace sw = bf3::sim::weapons;

namespace {

constexpr sim::Tick kMaxAheadTicks = sim::Tick(3600) * sim::kSimHz;

constexpr std::string_view kTeam1NameSid = "ID_M_US";
constexpr std::string_view kTeam2NameSid = "ID_M_RU";

float seconds_until(sim::Tick p_now, sim::Tick p_until, uint64_t &r_bad) {
	if (p_until <= p_now) {
		return 0.0f;
	}
	sim::Tick d = p_until - p_now;
	if (d > kMaxAheadTicks) {
		++r_bad;
		d = kMaxAheadTicks;
	}
	return float(double(d) / double(sim::kSimHz));
}

struct Rank {
	uint8_t slot;
	int32_t score;
	int16_t kills;
};

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

void push_kill(ui::HudKillFeed &r_feed, uint32_t &r_serial, const ui::HudKill &p_line) {
	++r_serial;
	if (r_serial == 0) {
		r_serial = 1;
	}
	ui::HudKill line = p_line;
	line.serial = r_serial;
	r_feed.lines[r_serial % r_feed.lines.size()] = line;
	r_feed.newest_serial = r_serial;
}

void hide_m2b(ui::HudModel &r_model) {
	r_model.kill_card.visible = false;
	r_model.kill_card.seconds_left = 0.0f;
	r_model.end_of_round.visible = false;
	r_model.spawn_screen.visible = false;
	r_model.pre_round.visible = false;
}

}

void fill_hud_tdm(const HudFeedInput &p_input, std::span<const PresentEvent> p_drained, const HudStatic &p_static, HudTdmFeedState &r_state,
		ui::HudModel &r_model) {
	++r_state.frames;
	if (!p_input.m2b) {
		hide_m2b(r_model);
		return;
	}

	const bool match = p_input.match_valid;
	net::MatchPhase phase = match ? p_input.match.phase : net::MatchPhase::Loading;
	if (uint8_t(phase) > uint8_t(net::MatchPhase::RoundOver)) {
		++r_state.bad_inputs;
		phase = net::MatchPhase::Loading;
	}
	const sim::Tick now_tick = p_input.server_tick;

	uint8_t own_slot = p_input.own_slot;
	if (own_slot != ui::kHudNoPlayer && own_slot >= ui::kHudMaxPlayers) {
		++r_state.bad_inputs;
		own_slot = ui::kHudNoPlayer;
	}
	uint8_t own_team = p_input.own_team;
	if (own_team > 2) {
		++r_state.bad_inputs;
		own_team = 0;
	}
	const uint8_t my_team = own_team != 0 ? own_team : 1;
	const uint8_t enemy_team = uint8_t(3 - my_team);

	std::array<bool, ui::kHudMaxPlayers> present{};
	r_model.players.fill(ui::HudPlayer{});
	size_t highest = 0;
	for (const net::PlayerRow &row : p_input.players) {
		if (row.slot >= ui::kHudMaxPlayers) {
			++r_state.bad_inputs;
			continue;
		}
		ui::HudPlayer &p = r_model.players[row.slot];
		p = ui::HudPlayer{};
		const size_t bytes = std::min<size_t>(std::min<size_t>(row.name_bytes, row.name.size()), p.name.size() - 1);
		if (row.name_bytes > row.name.size()) {
			++r_state.bad_inputs;
		}
		std::memcpy(p.name.data(), row.name.data(), bytes);
		if (row.team > 2) {
			++r_state.bad_inputs;
		} else {
			p.team = int8_t(row.team);
		}
		if (row.kit >= kKitCount) {
			++r_state.bad_inputs;
		} else {
			p.kit = int8_t(row.kit);
		}
		p.alive = row.alive;
		p.bot = row.bot;
		p.local = row.slot == own_slot;
		p.kills = int16_t(std::min<uint32_t>(row.kills, 32767u));
		p.deaths = int16_t(std::min<uint32_t>(row.deaths, 32767u));
		p.score = row.score;
		p.ping_ms = row.ping_ms;
		present[row.slot] = true;
		highest = std::max<size_t>(highest, size_t(row.slot) + 1);
	}
	r_model.player_count = uint8_t(highest);
	r_model.local_player = own_slot;
	r_model.local_team = int8_t(own_team);

	std::array<Rank, ui::kHudMaxPlayers> all{};
	std::array<Rank, ui::kHudMaxPlayers> t1{};
	std::array<Rank, ui::kHudMaxPlayers> t2{};
	size_t n_all = 0;
	size_t n1 = 0;
	size_t n2 = 0;
	for (size_t s = 0; s < ui::kHudMaxPlayers; ++s) {
		if (!present[s]) {
			continue;
		}
		const ui::HudPlayer &p = r_model.players[s];
		const Rank r{ uint8_t(s), p.score, p.kills };
		all[n_all++] = r;
		if (p.team == 1) {
			t1[n1++] = r;
		} else if (p.team == 2) {
			t2[n2++] = r;
		}
	}
	std::sort(all.begin(), all.begin() + n_all, rank_before);
	std::sort(t1.begin(), t1.begin() + n1, rank_before);
	std::sort(t2.begin(), t2.begin() + n2, rank_before);
	ui::HudTeam &team = r_model.team;
	team.active_teams_data = 2;
	set_list(team.sorted_players, std::span<const Rank>(all.data(), n_all));
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
			copy_name(r_model.players[all[i].slot], *tops[i]);
		}
	}
	uint8_t winner = match ? p_input.match.winner : 0;
	if (winner > 2) {
		++r_state.bad_inputs;
		winner = 0;
	}
	const bool round_over = phase == net::MatchPhase::RoundOver;
	team.friendly_team_is_winner = round_over && winner != 0 && winner == my_team;
	team.enemy_team1_is_winner = round_over && winner != 0 && winner == enemy_team;
	team.scoreboard_enabled = p_input.scoreboard_held;

	ui::HudKillCounter &kc = r_model.kill_counter;
	const int32_t k1 = match ? std::max(p_input.match.kills[0], 0) : 0;
	const int32_t k2 = match ? std::max(p_input.match.kills[1], 0) : 0;
	const int32_t limit = match ? std::max(p_input.match.kill_limit, 0) : 0;
	if (match && (p_input.match.kills[0] < 0 || p_input.match.kills[1] < 0 || p_input.match.kill_limit < 0)) {
		++r_state.bad_inputs;
	}
	kc.team1_kills = k1;
	kc.team2_kills = k2;
	kc.team_kills_list = {};
	kc.team_kills_list.count = 2;
	kc.team_kills_list.kills[0] = k1;
	kc.team_kills_list.kills[1] = k2;
	kc.team1_max_kills = limit;
	kc.team2_max_kills = limit;
	const int32_t mine = my_team == 1 ? k1 : k2;
	const int32_t theirs = my_team == 1 ? k2 : k1;
	kc.player_team = my_team;
	kc.player_team_kills = mine;
	kc.enemy_team_kills = theirs;
	kc.leading_team_kills = std::max(k1, k2);
	kc.leading_by_kills = mine - theirs;
	kc.player_team_position = mine >= theirs ? 1 : 2;

	if (!std::isfinite(p_input.now_s)) {
		++r_state.bad_inputs;
	}
	const double now_s = std::isfinite(p_input.now_s) ? p_input.now_s : 0.0;
	ui::HudMessages &msg = r_model.messages;
	ui::HudKillCard &card = r_model.kill_card;
	for (const PresentEvent &e : p_drained) {
		if (e.kind == PresentKind::Spawn) {

			if (own_slot != ui::kHudNoPlayer && e.slot == own_slot) {
				r_state.kill_card_until_s = 0;
			}
			continue;
		}
		if (e.kind != PresentKind::Death) {
			continue;
		}
		if (e.aux >= ui::kHudMaxPlayers) {
			++r_state.bad_inputs;
			continue;
		}
		const uint8_t victim = e.aux;
		uint8_t killer = e.slot;
		if (killer >= ui::kHudMaxPlayers) {
			++r_state.bad_inputs;
			killer = ui::kHudNoPlayer;
		}
		const bool suicide = killer == victim || killer == ui::kHudNoPlayer;
		const HudStaticRow *row = e.def != sw::kNoWeapon ? p_static.row(e.def) : nullptr;
		if (e.def != sw::kNoWeapon && row == nullptr) {
			++r_state.unknown_defs;
		}
		ui::HudKill line;
		line.killer = killer;
		line.victim = victim;
		line.item_id = row != nullptr ? row->item_id : 0;
		line.headshot = (e.flags & present_flags::kHeadshot) != 0;
		line.suicide = suicide;
		const int8_t kt = killer != ui::kHudNoPlayer && present[killer] ? r_model.players[killer].team : int8_t(0);
		const int8_t vt = present[victim] ? r_model.players[victim].team : int8_t(0);
		line.team_kill = !suicide && kt != 0 && kt == vt;
		push_kill(msg.kill_message, r_state.kill_serial, line);
		++r_state.deaths_fed;
		if ((e.flags & present_flags::kOwnShooter) != 0 && !suicide) {
			push_kill(msg.local_kill_message, r_state.local_kill_serial, line);
		}
		if ((e.flags & present_flags::kOwnVictim) == 0) {
			continue;
		}

		++r_state.kill_card_serial;
		if (r_state.kill_card_serial == 0) {
			r_state.kill_card_serial = 1;
		}
		++r_state.own_deaths;
		r_state.kill_card_until_s = now_s + kKillCardSeconds;
		r_state.own_death_respawn_tick = e.extra;
		r_state.own_death_tick = e.time > 0 ? sim::Tick(std::min<int64_t>(e.time / sw::kTimeUnitsPerTick, int64_t(UINT32_MAX))) : 0;
		card = ui::HudKillCard{};
		card.serial = r_state.kill_card_serial;
		card.killer = suicide ? ui::kHudNoPlayer : killer;
		card.killer_kit = !suicide && present[killer] ? r_model.players[killer].kit : int8_t(-1);
		card.headshot = line.headshot;
		card.suicide = suicide;
		card.item_id = row != nullptr ? row->item_id : 0;
		card.weapon_name_sid_hash = row != nullptr ? row->name_sid_hash : 0;
		const float hp = std::isfinite(e.value) ? std::clamp(e.value, 0.0f, 100.0f) : 0.0f;
		if (!std::isfinite(e.value) || e.value < 0.0f || e.value > 100.0f) {
			++r_state.bad_inputs;
		}
		card.killer_health = suicide ? 0.0f : hp;
		const HudKillCardSource &src = p_input.kill_card_source;
		card.distance_m = !suicide && src.killer_seen && std::isfinite(src.distance_m) && src.distance_m >= 0.0f ? src.distance_m : -1.0f;
	}
	const bool card_up = card.serial != 0 && now_s < r_state.kill_card_until_s;
	card.visible = card_up;
	card.seconds_left = card_up ? float(r_state.kill_card_until_s - now_s) : 0.0f;

	const bool dead = !p_input.alive || p_input.own_unspawned;
	ui::HudSpawnScreen &sp = r_model.spawn_screen;
	sp.visible = match && dead && !card_up && !round_over;
	sp.at_join = !p_input.spawned_this_round;
	uint8_t kit = p_input.spawn_kit;
	if (kit >= kKitCount) {
		++r_state.bad_inputs;
		kit = 0;
	}
	sp.selected_kit = kit;
	const bool deploy_phase = phase == net::MatchPhase::PreRound || phase == net::MatchPhase::InRound;
	sp.can_deploy = match && dead && deploy_phase && now_tick >= p_input.own_respawn_tick;
	sp.seconds_to_deploy = sp.can_deploy ? 0.0f : seconds_until(now_tick, p_input.own_respawn_tick, r_state.bad_inputs);
	sp.revive_possible = match && dead && !sp.at_join && now_tick < p_input.own_revive_until;
	sp.seconds_revive_left = sp.revive_possible ? seconds_until(now_tick, p_input.own_revive_until, r_state.bad_inputs) : 0.0f;
	for (auto &items : sp.kit_items) {
		items.fill(0);
	}
	if (p_input.kits != nullptr) {
		for (uint8_t k = 0; k < kKitCount; ++k) {
			const KitLoadout &l = p_input.kits->loadout(my_team, k);
			for (size_t i = 0; i < kHudSpawnSlots.size(); ++i) {
				const uint16_t def = l.defs[kHudSpawnSlots[i]];
				const HudStaticRow *r = def != sw::kNoWeapon ? p_static.row(def) : nullptr;
				sp.kit_items[k][i] = r != nullptr ? r->item_id : 0;
			}
		}
	}

	r_model.soldier.show_medic_revive = sp.revive_possible;
	r_model.soldier.time_until_revive = sp.seconds_revive_left;
	r_model.soldier.time_until_death = 0.0f;
	r_model.soldier.is_interactive_man_down = false;
	r_model.spawn_logic.time_to_respawn = sp.seconds_to_deploy;
	const sim::Tick d0 = r_state.own_death_tick;
	const sim::Tick d1 = r_state.own_death_respawn_tick;
	r_model.spawn_logic.start_time_to_respawn = d1 > d0 && d1 - d0 <= kMaxAheadTicks ? float(double(d1 - d0) / double(sim::kSimHz)) : 0.0f;
	r_model.spawn_points.can_spawn = sp.can_deploy;
	r_model.spawn_points.has_valid_spawn = match;

	ui::HudPreRound &pre = r_model.pre_round;
	pre.visible = phase == net::MatchPhase::PreRound;
	pre.counting_down = match && p_input.match.countdown_end_tick != 0;
	pre.seconds_left = pre.counting_down ? seconds_until(now_tick, p_input.match.countdown_end_tick, r_state.bad_inputs) : 0.0f;
	pre.input_restricted = match && p_input.match.restrict_until_tick != 0 && now_tick < p_input.match.restrict_until_tick;

	ui::HudEndOfRound &eor = r_model.end_of_round;
	eor.visible = round_over;
	eor.winner = int8_t(winner);
	eor.local_won = round_over && own_team != 0 && winner == own_team;
	eor.team_kills = { k1, k2 };
	eor.kill_limit = limit;
	for (size_t i = 0; i < eor.top_players.size(); ++i) {
		eor.top_players[i] = i < n_all ? all[i].slot : ui::kHudNoPlayer;
	}
	eor.seconds_to_next_round = match && p_input.match.restart_tick != 0 ? seconds_until(now_tick, p_input.match.restart_tick, r_state.bad_inputs) : 0.0f;
}

}
