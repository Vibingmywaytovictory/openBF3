#include "hud_feed_tdm_parts.h"

#include <algorithm>
#include <cmath>

namespace bf3::host::tdm_feed {

namespace ui = bf3::core::ui;

namespace {

int32_t knife_class() {
	static const int32_t c = hud_weapon_class("wcKnife");
	return c;
}

float round_seconds(const Frame &p_frame) {
	const net::MatchState &ms = p_frame.input.match;
	if (!p_frame.match || ms.round_start_tick == 0) {
		return 0.0f;
	}
	const sim::Tick end = p_frame.round_over && ms.round_end_tick != 0 ? ms.round_end_tick : p_frame.now_tick;
	if (end <= ms.round_start_tick) {
		return 0.0f;
	}
	return float(double(std::min<sim::Tick>(end - ms.round_start_tick, kMaxAheadTicks)) / double(sim::kSimHz));
}

void feed_eor_stage(const Frame &p_frame, ui::HudEndOfRound &r_eor) {
	r_eor.stage = ui::HudEorStage::None;
	r_eor.stage_seconds = 0.0f;
	const core::gameplay::EorTimeline *t = p_frame.input.eor;
	const sim::Tick unspawn = p_frame.input.match.unspawn_tick;
	if (!p_frame.round_over || !p_frame.match || t == nullptr || unspawn == 0 || p_frame.now_tick < unspawn) {
		return;
	}
	const double since = double(std::min<sim::Tick>(p_frame.now_tick - unspawn, kMaxAheadTicks)) / double(sim::kSimHz);
	const double pre_eor = double(t->fade_s);
	const double scoreboard = pre_eor + double(t->scoreboard_delay_s);
	const double eor = std::max(double(t->pre_eor_s), pre_eor + double(t->request_eor_delay_s));
	if (since < pre_eor) {
		r_eor.stage = ui::HudEorStage::Fade;
		r_eor.stage_seconds = float(since);
	} else if (since < scoreboard) {
		r_eor.stage = ui::HudEorStage::PreEor;
		r_eor.stage_seconds = float(since - pre_eor);
	} else if (since < eor) {
		r_eor.stage = ui::HudEorStage::Scoreboard;
		r_eor.stage_seconds = float(since - scoreboard);
	} else {
		r_eor.stage = ui::HudEorStage::Eor;
		r_eor.stage_seconds = float(since - eor);
	}
}

void feed_eor_stats(Frame &r_frame, ui::HudEndOfRound &r_eor) {
	const RoundStats &st = r_frame.state.stats;
	const ui::HudModel &model = r_frame.model;
	const uint8_t own = r_frame.own_slot;
	const bool have_own = own != ui::kHudNoPlayer && r_frame.present[own];
	r_eor.winning_team = int32_t(r_frame.winner);
	r_eor.local_player_in_winning_team = r_eor.local_won;
	r_eor.time_until_level_change = r_eor.seconds_to_next_round;
	r_eor.total_score = have_own ? model.players[own].score : 0;
	r_eor.combat_score = r_eor.total_score;
	r_eor.kills = int32_t(st.own_kills);
	r_eor.deaths = int32_t(st.own_deaths);
	r_eor.kill_death_ratio =
			st.own_deaths != 0 ? float(double(st.own_kills) / double(st.own_deaths)) : float(st.own_kills);
	const RoundBestWeapon best = round_stats_best_weapon(st);
	r_eor.best_weapon_info = ui::HudBestWeapon{};
	if (const HudStaticRow *row = best.kills != 0 ? r_frame.statics.row(best.def) : nullptr) {
		r_eor.best_weapon_info.item_id = row->item_id;
		r_eor.best_weapon_info.name_sid_hash = row->name_sid_hash;
		r_eor.best_weapon_info.kills = best.kills;
		if (const HudStaticItem *item = r_frame.statics.item(row->item_id)) {
			r_eor.best_weapon_info.image_path = item->texture_path;
		}
	}
	int32_t dogtags = 0;
	for (size_t d = 0; d < st.own_weapon_kills.size(); ++d) {
		const HudStaticRow *row = r_frame.statics.row(uint16_t(d));
		dogtags += row != nullptr && row->weapon_class == knife_class() ? int32_t(st.own_weapon_kills[d]) : 0;
	}
	r_eor.dogtags_taken = dogtags;
	r_eor.round_duration = round_seconds(r_frame);
	r_eor.total_playtime = 0.0f;
	r_eor.kills_per_minute = round_stats_per_minute(double(st.own_kills), r_eor.round_duration);
	r_eor.score_per_minute =
			round_stats_per_minute(double(r_eor.total_score), std::max(60.0, double(r_eor.total_playtime)));
	r_eor.longest_streak = int32_t(st.longest_streak);
	r_eor.longest_nemesis_streak = int32_t(st.longest_nemesis_streak);
	r_eor.longest_headshot = st.longest_headshot_m;
	r_eor.name = have_own ? model.players[own].name : ui::HudPlayerName{};
	r_eor.team_results = { r_frame.team1_kills, r_frame.team2_kills };
	r_eor.player_team_index = int32_t(r_frame.own_team);
	r_eor.in_match = r_frame.match;
	r_eor.is_ranked = false;
	r_eor.is_spectator = false;
}

}

void feed_round_stats(Frame &r_frame, std::span<const PresentEvent> p_drained) {
	HudTdmFeedState &state = r_frame.state;
	const net::MatchPhase last = state.stats_phase;
	const bool was_over = last == net::MatchPhase::RoundOver || last == net::MatchPhase::Restarting;
	if (was_over && !r_frame.round_over) {
		round_stats_reset(state.stats, r_frame.own_slot);
	}
	state.stats_phase = r_frame.phase;
	for (size_t s = 0; s < r_frame.teams.size(); ++s) {
		r_frame.teams[s] = r_frame.present[s] ? r_frame.model.players[s].team : int8_t(0);
	}
	RoundStatsInput in;
	in.own_slot = r_frame.own_slot < kRoundStatsSlots ? r_frame.own_slot : kRoundStatsNoSlot;
	in.own_position = r_frame.input.own_position;
	in.others = r_frame.input.others;
	in.teams = r_frame.teams;
	round_stats_add(state.stats, in, p_drained);
}

void feed_round_end(Frame &r_frame) {
	const HudFeedInput &in = r_frame.input;
	HudTdmFeedState &state = r_frame.state;
	ui::HudModel &model = r_frame.model;
	const bool match = r_frame.match;
	const sim::Tick now_tick = r_frame.now_tick;

	ui::HudPreRound &pre = model.pre_round;
	pre.visible = r_frame.phase == net::MatchPhase::PreRound;
	pre.counting_down = match && in.match.countdown_end_tick != 0;
	pre.seconds_left =
			pre.counting_down ? seconds_until(now_tick, in.match.countdown_end_tick, state.bad_inputs) : 0.0f;
	pre.input_restricted = match && in.match.restrict_until_tick != 0 && now_tick < in.match.restrict_until_tick;
	pre.players_needed = pre.visible && match ? int32_t(in.match.players_needed) : 0;
	pre.count_left = pre.counting_down ? int32_t(std::ceil(pre.seconds_left)) : pre.players_needed;

	ui::HudEndOfRound &eor = model.end_of_round;
	eor.visible = r_frame.round_over;
	eor.winner = int8_t(r_frame.winner);
	eor.local_won = r_frame.round_over && r_frame.own_team != 0 && r_frame.winner == r_frame.own_team;
	eor.team_kills = { r_frame.team1_kills, r_frame.team2_kills };
	eor.kill_limit = r_frame.kill_limit;
	for (size_t i = 0; i < eor.top_players.size(); ++i) {
		eor.top_players[i] = i < r_frame.ranked_count ? r_frame.ranked[i].slot : ui::kHudNoPlayer;
	}
	eor.seconds_to_next_round = match && in.match.restart_tick != 0
			? seconds_until(now_tick, in.match.restart_tick, state.bad_inputs)
			: 0.0f;
	feed_eor_stage(r_frame, eor);
	feed_eor_stats(r_frame, eor);
}

}
