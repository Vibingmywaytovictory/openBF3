#include "hud_feed_tdm_parts.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace ui = bf3::core::ui;

namespace tdm_feed {

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

}

namespace {

void hide_tdm(ui::HudModel &r_model) {
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
	if (!p_input.tdm) {
		hide_tdm(r_model);
		return;
	}
	tdm_feed::Frame f{ p_input, p_static, r_state, r_model };
	f.match = p_input.match_valid;
	f.phase = f.match ? p_input.match.phase : net::MatchPhase::Loading;
	if (uint8_t(f.phase) > uint8_t(net::MatchPhase::Restarting)) {
		++r_state.bad_inputs;
		f.phase = net::MatchPhase::Loading;
	}
	f.now_tick = p_input.server_tick;
	f.own_slot = p_input.own_slot;
	if (f.own_slot != ui::kHudNoPlayer && f.own_slot >= ui::kHudMaxPlayers) {
		++r_state.bad_inputs;
		f.own_slot = ui::kHudNoPlayer;
	}
	f.own_team = p_input.own_team;
	if (f.own_team > 2) {
		++r_state.bad_inputs;
		f.own_team = 0;
	}
	f.my_team = f.own_team != 0 ? f.own_team : 1;
	f.enemy_team = uint8_t(3 - f.my_team);
	f.winner = f.match ? p_input.match.winner : 0;
	if (f.winner > 2) {
		++r_state.bad_inputs;
		f.winner = 0;
	}
	f.round_over = f.phase == net::MatchPhase::RoundOver || f.phase == net::MatchPhase::Restarting;
	f.team1_kills = f.match ? std::max(p_input.match.kills[0], 0) : 0;
	f.team2_kills = f.match ? std::max(p_input.match.kills[1], 0) : 0;
	f.kill_limit = f.match ? std::max(p_input.match.kill_limit, 0) : 0;
	if (f.match && (p_input.match.kills[0] < 0 || p_input.match.kills[1] < 0 || p_input.match.kill_limit < 0)) {
		++r_state.bad_inputs;
	}
	if (!std::isfinite(p_input.now_s)) {
		++r_state.bad_inputs;
	}
	f.now_s = std::isfinite(p_input.now_s) ? p_input.now_s : 0.0;

	tdm_feed::feed_scoreboard(f);
	tdm_feed::feed_round_stats(f, p_drained);
	tdm_feed::feed_kill_card(f, p_drained);
	tdm_feed::feed_spawn(f);
	tdm_feed::feed_round_end(f);
}

}
