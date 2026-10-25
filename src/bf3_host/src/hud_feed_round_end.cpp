#include "hud_feed_tdm_parts.h"

namespace bf3::host::tdm_feed {

namespace ui = bf3::core::ui;

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
}

}
