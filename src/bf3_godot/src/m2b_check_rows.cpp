#include "m2b_check_rows.h"

#include "bf3/core/util/format.h"

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

using bf3::core::fmt;

bool m2b_death_had_screen_time(double p_death_t, double p_now_t, double p_round_over_t, double p_need_s) {
	const double end = p_round_over_t >= 0.0 ? std::min(p_now_t, p_round_over_t) : p_now_t;
	return end - p_death_t >= p_need_s;
}

M2bSeekSpot m2b_seek_spot(const sim::Vec3 &p_enemy, float p_enemy_yaw, float p_metres) {
	M2bSeekSpot r;
	r.position = p_enemy;
	if (!std::isfinite(p_enemy_yaw) || !std::isfinite(p_metres) || !std::isfinite(p_enemy[0]) || !std::isfinite(p_enemy[1]) || !std::isfinite(p_enemy[2])) {
		r.yaw = 0.0f;
		return r;
	}
	r.position[0] += std::sin(p_enemy_yaw) * p_metres;
	r.position[2] += std::cos(p_enemy_yaw) * p_metres;
	constexpr float kPi = 3.14159265358979323846f;
	r.yaw = p_enemy_yaw + kPi;
	if (r.yaw > kPi) {
		r.yaw -= 2.0f * kPi;
	}
	return r;
}

std::vector<M2bPlayRow> m2b_play_rows(const M2bPlayTally &p_t, bool p_have_ui, bool p_have_cam) {
	std::vector<M2bPlayRow> rows;
	const auto add = [&](M2bTrack p_track, const std::string &p_name, double p_value, const std::string &p_rule, bool p_pass, bool p_window_only = false) {
		M2bPlayRow r;
		r.name = p_name;
		r.value = p_value;
		r.rule = p_rule;
		r.track = p_track;
		if (p_track == M2bTrack::Ui && !p_have_ui) {
			r.skip_reason = "waiting for ui";
		} else if (p_track == M2bTrack::Cam && !p_have_cam) {
			r.skip_reason = "waiting for cam";
		} else if (p_window_only && !p_t.windowed) {
			r.skip_reason = "headless: nothing is drawn";
		}
		r.state = !r.skip_reason.empty() ? M2bRowState::Skip : (p_pass ? M2bRowState::Pass : M2bRowState::Fail);
		rows.push_back(std::move(r));
	};

	add(M2bTrack::Gx, "match_state", p_t.in_round_seen ? 1.0 : 0.0, "MatchState received and InRound reached", p_t.match_seen && p_t.in_round_seen);
	add(M2bTrack::Gx, "spawn_screen_join", p_t.join_screen_seen ? 1.0 : 0.0, "the spawn screen is up at join", p_t.join_screen_seen);
	add(M2bTrack::Gx, "own_deaths", double(p_t.own_deaths), ">= 1 own death in the run", p_t.own_deaths >= 1);
	add(M2bTrack::Gx, "spawn_screen_after_death", double(p_t.screens_after_death),
			fmt("== the %g own deaths with time to show it (of %g)", double(p_t.deaths_checked),
					double(p_t.own_deaths)),
			p_t.deaths_checked >= 1 && p_t.screens_after_death == p_t.deaths_checked);
	add(M2bTrack::Gx, "deploys", double(p_t.deploys), ">= 2 (at join and after a death)", p_t.deploys >= 2);
	add(M2bTrack::Gx, "deploy_kit_mismatch", double(p_t.deploy_kit_mismatch), "== 0: each deploy spawns the chosen kit", p_t.deploys >= 1 && p_t.deploy_kit_mismatch == 0);
	add(M2bTrack::Gx, "round_over", p_t.round_over_seen ? 1.0 : 0.0, "MatchState reached RoundOver (--kill-limit)", p_t.round_over_seen);

	add(M2bTrack::Ui, "kill_feed_lines", double(p_t.feed_lines),
			fmt("== the %g deaths received", double(p_t.deaths_received)),
			p_t.deaths_received >= 1 && p_t.feed_lines == p_t.deaths_received);
	add(M2bTrack::Ui, "scoreboard_kills_mismatch", double(p_t.scoreboard_mismatch),
			fmt("== 0 of %g Tab samples against MatchState", double(p_t.scoreboard_samples)),
			p_t.scoreboard_samples >= 1 && p_t.scoreboard_mismatch == 0);
	add(M2bTrack::Ui, "scoreboard_hidden_on_release", double(p_t.scoreboard_release_shown),
			fmt("== 0 of %g samples half a hold after Tab is released", double(p_t.scoreboard_release_samples)),
			p_t.scoreboard_release_samples >= 1 && p_t.scoreboard_release_shown == 0);
	add(M2bTrack::Ui, "scoreboard_round_over", double(p_t.scoreboard_round_over_shown),
			fmt("== 0 of %g Tab holds under the round-end banner show the scoreboard", double(p_t.scoreboard_round_over_samples)),
			p_t.scoreboard_round_over_samples >= 1 && p_t.scoreboard_round_over_shown == 0, true);
	add(M2bTrack::Ui, "kill_card_bad", double(p_t.kill_cards_bad),
			fmt("== 0 of %g kill cards without the killer or the weapon", double(p_t.kill_cards_checked)),
			p_t.kill_cards_checked >= 1 && p_t.kill_cards_bad == 0);
	add(M2bTrack::Ui, "spawn_screen_shown", double(p_t.spawn_screen_model_shown),
			fmt(">= 1 + the %g checked deaths", double(p_t.deaths_checked)),
			p_t.spawn_screen_model_shown >= 1 + p_t.deaths_checked);
	add(M2bTrack::Ui, "round_end_screen", p_t.round_end_shown ? 1.0 : 0.0, "the round-end screen on RoundOver", p_t.round_over_seen && p_t.round_end_shown);
	add(M2bTrack::Ui, "nametag_icons", double(p_t.tag_icons_painted),
			fmt(">= 1 native nametag icon painted and >= 1 teammate on the minimap (%g teammate icons over %g frames)", double(p_t.minimap_friendly),
					double(p_t.tag_frames)),
			p_t.tag_frames >= 1 && p_t.tag_icons_painted >= 1 && p_t.minimap_friendly >= 1, true);

	add(M2bTrack::Cam, "death_views", double(p_t.death_views),
			fmt(">= the %g own deaths, %g bad inputs", double(p_t.own_deaths), double(p_t.death_view_bad_inputs)),
			p_t.own_deaths >= 1 && p_t.death_views >= p_t.own_deaths && p_t.death_view_bad_inputs == 0);
	return rows;
}

}
