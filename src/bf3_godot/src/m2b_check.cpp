#include "m2b_check.h"
#include "m2b_check_rows.h"

#include "level_session_game.h"
#include "level_session_tdm.h"

#include "godot_util.h"
#include "hud_items.h"
#include "input_mapper.h"

#include "bf3/core/util/format.h"
#include "bf3/host/hud_feed.h"
#include "bf3/net/protocol.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;
namespace net = bf3::net;
namespace sim = bf3::sim;

namespace {

using bf3::core::fmt;

constexpr double kM2bJoinShotDelayS = 1.5;
constexpr double kM2bChooseDelayS = 0.6;
constexpr double kM2bTabEveryS = 9.0;
constexpr double kM2bTabHoldS = 0.6;

constexpr double kM2bSeekEnemyAfterS = 6.0;
constexpr double kM2bSeekEveryS = 4.0;
constexpr float kM2bSeekFrontMetres = 6.0f;
constexpr double kM2bFallAfterS = 26.0;
constexpr double kM2bScreenGraceS = 4.0;
constexpr double kM2bRoundOverHoldS = 3.0;
constexpr double kM2bHardEndAfterS = 45.0;

}

struct M2bCheck::State {
	Bf3LevelSession &s;
	GameState &g;
	TdmState &m;
	std::vector<M2bCase> cases;
	M2bCheckOptions options;
	std::vector<M2bCaseResult> results;
	std::vector<M2bPlayRow> play_rows;
	size_t next_case = 0;
	bool case_running = false;
	double case_t0 = -1.0;
	M2bPlayTally tally;

	bool join_counted = false;
	double screen_up_t = -1.0;
	bool screen_was_up = false;
	bool kit_pressed = false, deploy_pressed = false;
	uint32_t deploy_index = 0;
	uint32_t lives_seen = 0;
	uint32_t life_seen = 0;
	bool was_alive = false;
	double life_t0 = -1.0;
	double next_seek = -1.0;
	uint8_t last_sought = 0xFF;
	bool dropped = false;
	double tab_until = -1.0, next_tab = 0.0;
	bool tab_shot = false;

	uint32_t deaths_seen = 0;
	bool death_pending = false;
	double death_t = 0.0;
	bool death_screen_seen = false;
	bool death_by_soldier = false;
	bool killcard_checked = false;
	bool killcard_shot = false, spawn_shot = false, join_shot = false, round_end_shot = false;
	double round_over_t = -1.0;

	bool model_spawn_visible = false;
	uint32_t shots_saved = 0;

	State(Bf3LevelSession &r_s, GameState &r_g, TdmState &r_m) :
			s(r_s), g(r_g), m(r_m) {}

	const host::PresentationFrame &f() const { return s.frame(); }
	InputMapper &mapper() { return s.mapper(); }
	void note(const std::string &p_line) { UtilityFunctions::print(to_godot("[m2b] " + p_line)); }
	bool save_shot(const char *p_name) {
		if (options.shots_dir.empty() || !tally.windowed) {
			return false;
		}
		const std::u8string dir = options.shots_dir.u8string();
		const std::string path = std::string(dir.begin(), dir.end()) + "/" + p_name + ".png";
		if (s.save_screenshot(path)) {
			++shots_saved;
			note("screenshot " + path);
			return true;
		}
		return false;
	}
	void send_teleport(const sim::Vec3 &p_position, float p_yaw) {
		host::ClientSession *c = s.client();
		if (c == nullptr) {
			return;
		}
		net::DebugRequest q;
		q.kind = net::DebugKind::Teleport;
		q.position = p_position;
		q.yaw = sim::quantize_yaw(p_yaw);
		mapper().set_requested_pose(sim::Pose::Stand);
		mapper().script_view(p_yaw, 0.0f);
		c->send_debug(q);
	}

	void resolve_death(bool p_revived, double p_t) {
		if (!death_pending) {
			return;
		}
		death_pending = false;
		const bool had_time = m2b_death_had_screen_time(death_t, p_t, round_over_t, host::kKillCardSeconds + kM2bScreenGraceS);
		if (death_screen_seen) {
			++tally.deaths_checked;
			++tally.screens_after_death;
		} else if (!p_revived && had_time) {
			++tally.deaths_checked;
			note(fmt("own death at %.1f s: no spawn screen after it", death_t));
		}
	}

	void play_frame(double p_t, uint64_t p_now_ns) {
		const host::PresentationFrame &fr = f();
		InputMapper &mp = mapper();
		const double now_s = double(p_now_ns) * 1e-9;

		if (fr.match_valid) {
			tally.match_seen = true;
			tally.in_round_seen = tally.in_round_seen || fr.match.phase == net::MatchPhase::InRound;
			if (fr.match.phase == net::MatchPhase::RoundOver && round_over_t < 0.0) {
				round_over_t = p_t;
				tally.round_over_seen = true;
				note(fmt("round over at %.1f s: US %d - RU %d (winner %d)", p_t, fr.match.kills[0], fr.match.kills[1],
						int(fr.match.winner)));
			}
		}

		if (m.spawn_screen && !m.spawned_this_round && lives_seen == 0) {
			if (!join_counted) {
				join_counted = true;
				tally.join_screen_seen = true;
				note(fmt("spawn screen at join (%.1f s)", p_t));
			}
			if (!join_shot && p_t >= kM2bJoinShotDelayS) {
				join_shot = save_shot("gx_m2b_join") || !tally.windowed;
			}
		}

		if (g.alive && (fr.own_curr.life != life_seen || !was_alive)) {
			const bool revived = m.life_revived;
			life_seen = fr.own_curr.life;
			++lives_seen;
			life_t0 = p_t;
			next_seek = p_t + kM2bSeekEnemyAfterS;
			dropped = false;
			resolve_death(revived, p_t);
			if (!revived) {
				tally.kits_deployed_mask |= 1u << std::min<uint8_t>(fr.own_kit, 3);
			}
			note(fmt("life %u at %.1f s: %s, kit %u, team %u", lives_seen, p_t, revived ? "revived" : "deployed",
					unsigned(fr.own_kit), unsigned(m.own_team)));
			mp.release_all();
			mp.script_hold(InputMapper::TdmAction::Scoreboard, false);
			next_tab = p_t + 2.0;
		}
		was_alive = g.alive;

		if (m.own_deaths > deaths_seen) {
			deaths_seen = m.own_deaths;
			resolve_death(false, p_t);
			death_pending = true;
			death_t = p_t;
			death_screen_seen = false;
			death_by_soldier = m.own_killer != 0xFF;
			killcard_checked = false;
			mp.release_all();
			mp.script_hold(InputMapper::TdmAction::Scoreboard, false);
			tab_until = -1.0;
			note(fmt("own death %u at %.1f s (killer %s)", deaths_seen, p_t,
					death_by_soldier ? std::to_string(m.own_killer).c_str() : "none"));
		}
		if (death_pending && m.spawn_screen) {
			death_screen_seen = true;
		}

		if (m.spawn_screen && !screen_was_up) {
			screen_up_t = p_t;
			kit_pressed = deploy_pressed = false;
		}
		screen_was_up = m.spawn_screen;
		const bool round_over = fr.match_valid && fr.match.phase == net::MatchPhase::RoundOver;
		if (m.spawn_screen && !round_over) {
			if (death_pending && !spawn_shot && p_t - screen_up_t >= 0.3) {
				spawn_shot = save_shot("gx_m2b_spawn") || !tally.windowed;
			}
			const bool join_wait = lives_seen == 0 && !join_shot && tally.windowed;
			if (!kit_pressed && p_t - screen_up_t >= kM2bChooseDelayS && !join_wait) {
				const uint8_t kit = uint8_t(deploy_index % 4);
				mp.script_press(InputMapper::TdmAction(uint8_t(InputMapper::TdmAction::Kit1) + kit));
				kit_pressed = true;
			}
			if (kit_pressed && !deploy_pressed && m.can_deploy && p_t - screen_up_t >= 2.0 * kM2bChooseDelayS) {
				mp.script_press(InputMapper::TdmAction::Deploy);
				deploy_pressed = true;
				++deploy_index;
			}
		}

		if (round_over && tab_until >= 0.0) {
			mp.script_hold(InputMapper::TdmAction::Scoreboard, false);
			tab_until = -1.0;
		}

		if (g.alive && !round_over) {
			if (tab_until < 0.0 && p_t >= next_tab) {
				tab_until = p_t + kM2bTabHoldS;
				mp.script_hold(InputMapper::TdmAction::Scoreboard, true);
			}
			if (tab_until >= 0.0 && p_t >= tab_until) {
				mp.script_hold(InputMapper::TdmAction::Scoreboard, false);
				tab_until = -1.0;
				next_tab = p_t + kM2bTabEveryS;
			}
			if (tab_until >= 0.0 && p_t >= tab_until - kM2bTabHoldS * 0.5) {
				sample_scoreboard();
				if (!tab_shot) {
					tab_shot = save_shot("gx_m2b_scoreboard") || !tally.windowed;
				}
			}
			const bool in_round = fr.match_valid && fr.match.phase == net::MatchPhase::InRound;

			const bool need_death = m.own_deaths == 0 || m.deploys < 2;
			if (in_round && need_death && next_seek >= 0.0 && p_t >= next_seek) {
				next_seek = p_t + kM2bSeekEveryS;
				seek_enemy(p_now_ns, p_t);
			}
			if (in_round && m.own_deaths == 0 && !dropped && p_t - life_t0 >= kM2bFallAfterS) {
				dropped = true;
				sim::Vec3 up = fr.own_curr.position;
				up[1] += 60.0f;
				send_teleport(up, mp.view().yaw);
				note(fmt("still alive after %.0f s: a 60 m drop", p_t - life_t0));
			}
		}

		ui_tally(now_s, p_t);

		if (round_over_t >= 0.0 && !round_end_shot && p_t - round_over_t >= 1.0) {
			round_end_shot = save_shot("gx_m2b_round_end") || !tally.windowed;
		}
	}

	void seek_enemy(uint64_t p_now_ns, double p_t) {
		const host::PresentationFrame &fr = f();
		std::array<host::PresentedSoldier, net::kMaxSlots> others{};
		const size_t n = fr.valid ? host::present_others(fr, p_now_ns, others) : 0;
		const sim::Vec3 &me = fr.own_curr.position;

		int best = -1, best_other = -1;
		float best_d = 1e30f, best_other_d = 1e30f;
		for (size_t i = 0; i < n; ++i) {
			const uint8_t team = g.slot_team[others[i].slot];
			if (team == 0 || team == m.own_team) {
				continue;
			}
			const float dx = others[i].position[0] - me[0], dz = others[i].position[2] - me[2];
			const float d = dx * dx + dz * dz;
			if (d < best_d) {
				best_d = d;
				best = int(i);
			}
			if (others[i].slot != last_sought && d < best_other_d) {
				best_other_d = d;
				best_other = int(i);
			}
		}
		if (best_other >= 0) {
			best = best_other;
		}
		if (best < 0) {
			note(fmt("%.1f s: no enemy in view to walk to", p_t));
			return;
		}

		const host::PresentedSoldier &e = others[size_t(best)];
		const M2bSeekSpot spot = m2b_seek_spot(e.position, e.yaw, kM2bSeekFrontMetres);
		last_sought = e.slot;
		send_teleport(spot.position, spot.yaw);
		note(fmt("%.1f s, alive %.0f s without an own death: teleported in front of enemy slot %u", p_t, p_t - life_t0,
				unsigned(e.slot)));
	}

	void sample_scoreboard() {
		const host::PresentationFrame &fr = f();
		if (!fr.match_valid || !m.scoreboard_held) {
			return;
		}
		int32_t k1 = g.hud_model.kill_counter.team1_kills, k2 = g.hud_model.kill_counter.team2_kills;
		bool shown = g.hud_model.team.scoreboard_enabled;
		if (m.tdm_hud != nullptr && m.tdm_hud->built() && tally.windowed) {
			const TdmHudRecords r = m.tdm_hud->records();
			shown = r.scoreboard_visible;
			k1 = r.scoreboard_team_kills[0];
			k2 = r.scoreboard_team_kills[1];
		}
		++tally.scoreboard_samples;

		const bool ok = shown && std::abs(k1 - fr.match.kills[0]) + std::abs(k2 - fr.match.kills[1]) <= 1;
		if (!ok) {
			++tally.scoreboard_mismatch;
			note(fmt("scoreboard %s: %d : %d, MatchState %d : %d", shown ? "shown" : "NOT shown", k1, k2,
					fr.match.kills[0], fr.match.kills[1]));
		}
	}

	void ui_tally(double p_now_s, double p_t) {
		(void)p_now_s;

		if (death_pending && !killcard_checked && p_t - death_t >= 0.2) {
			killcard_checked = true;
			if (death_by_soldier) {
				const core::ui::HudKillCard &kc = g.hud_model.kill_card;
				bool ok = kc.visible && kc.killer != core::ui::kHudNoPlayer && !kc.suicide && kc.item_id != 0;
				if (m.tdm_hud != nullptr && m.tdm_hud->built() && tally.windowed) {
					const TdmHudRecords r = m.tdm_hud->records();
					ok = ok && r.kill_card_visible && r.kill_card_killer[0] != 0 && r.kill_card_item != 0 && r.kill_card_weapon_named;
				}
				++tally.kill_cards_checked;
				if (!ok) {
					++tally.kill_cards_bad;
					note(fmt("kill card: visible %d, killer %u, item %u", int(kc.visible), unsigned(kc.killer),
							unsigned(kc.item_id)));
				}
			}
			if (!killcard_shot) {
				killcard_shot = save_shot("gx_m2b_killcard") || !tally.windowed;
			}
		}
		const bool visible = g.hud_model.spawn_screen.visible;
		if (visible && !model_spawn_visible) {
			++tally.spawn_screen_model_shown;
		}
		model_spawn_visible = visible;
		if (round_over_t >= 0.0 && g.hud_model.end_of_round.visible) {
			tally.round_end_shown = true;
		}
	}

	void finish_play(double p_t) {
		resolve_death(false, p_t);
		mapper().release_all();
		mapper().script_hold(InputMapper::TdmAction::Scoreboard, false);
		tally.seconds = p_t;
		tally.own_deaths = m.own_deaths;
		tally.deploys = m.deploys;
		tally.deploy_kit_mismatch = m.deploy_kit_mismatch;
		tally.spawn_requests = m.deploy.requests;
		tally.spawn_request_resends = m.deploy.resends;
		tally.deaths_received = m.deaths_received;
		tally.feed_lines = m.tdm_state.deaths_fed;
		if (m.tdm_hud != nullptr && m.tdm_hud->built() && tally.windowed) {
			const TdmHudRecords r = m.tdm_hud->records();
			tally.feed_lines = r.kill_feed_lines_started;
			tally.spawn_screen_model_shown = uint32_t(std::min<uint64_t>(r.spawn_screen_shown, 0xFFFFFFFFu));
			tally.round_end_shown = tally.round_end_shown && r.round_end_shown > 0;
			tally.missing_lookups = r.missing_fonts + r.missing_icons + r.missing_strings;
		} else if (tally.windowed) {
			tally.missing_lookups = 1;
		}
		if (m.death_camera != nullptr) {
			tally.death_views = m.death_camera->stats().views;
			tally.death_view_bad_inputs = m.death_camera->stats().bad_inputs;
		}
		play_rows = m2b_play_rows(tally, true, true);
		M2bCaseResult r;
		r.which = M2bCase::Play;
		std::string skipped;
		bool fail = false;
		for (const M2bPlayRow &row : play_rows) {
			if (row.state == M2bRowState::Skip) {
				skipped += (skipped.empty() ? "" : ", ") + row.name;
				continue;
			}
			M2aRow out;
			out.name = row.name;
			out.value = row.value;
			out.rule = row.rule;
			out.pass = row.state == M2bRowState::Pass;
			if (!out.pass && !fail) {
				fail = true;
				r.reason = row.name;
			}
			r.rows.push_back(std::move(out));
		}
		r.outcome = fail ? M2aOutcome::Fail : M2aOutcome::Pass;
		if (!fail && !skipped.empty()) {
			r.reason = "skipped: " + skipped;
		}
		results.push_back(std::move(r));
		note(fmt("play: %.1f s, %u own deaths, %u deploys (kits mask 0x%x, %llu requests, %llu resends), %llu deaths "
				 "received, %u screenshots",
				p_t, tally.own_deaths, tally.deploys, tally.kits_deployed_mask,
				(unsigned long long)tally.spawn_requests, (unsigned long long)tally.spawn_request_resends,
				(unsigned long long)tally.deaths_received, shots_saved));
	}
};

M2bCheck::M2bCheck(Bf3LevelSession &r_session, GameState &r_game, TdmState &r_m2b, std::vector<M2bCase> p_cases, const M2bCheckOptions &p_options) :
		state_(std::make_unique<State>(r_session, r_game, r_m2b)) {
	state_->cases = std::move(p_cases);
	state_->options = p_options;
	state_->tally.windowed = r_m2b.windowed;
	r_session.mapper().set_scripted(true);
	r_session.mapper().release_all();
	UtilityFunctions::print(to_godot(
			fmt("[m2b] check: %zu case(s), play %.0f s (timeout %.0f s), %s", state_->cases.size(),
					p_options.play_seconds, p_options.case_timeout_s, r_m2b.windowed ? "windowed" : "headless")));
}

M2bCheck::~M2bCheck() = default;

bool M2bCheck::frame(uint64_t p_now_ns, double p_dt) {
	(void)p_dt;
	State &st = *state_;
	const double now_s = double(p_now_ns) * 1e-9;
	if (st.next_case >= st.cases.size()) {
		return true;
	}
	if (!st.case_running) {
		st.case_running = true;
		st.case_t0 = now_s;
		st.note("case " + std::string(m2b_case_name(st.cases[st.next_case])));
	}
	const double t = now_s - st.case_t0;
	switch (st.cases[st.next_case]) {
		case M2bCase::Play: {
			st.play_frame(t, p_now_ns);
			const bool settled = !st.death_pending || t - st.death_t >= host::kKillCardSeconds + kM2bScreenGraceS;
			const bool over_long_enough = st.round_over_t >= 0.0 && t - st.round_over_t >= kM2bRoundOverHoldS && (st.round_end_shot || !st.tally.windowed);
			const bool enough = st.m.own_deaths >= 1 && st.m.deploys >= 2 && settled;
			const bool end = (over_long_enough && (enough || t >= st.options.play_seconds)) || t >= st.options.play_seconds + kM2bHardEndAfterS ||
					t >= st.options.case_timeout_s;
			if (end) {
				st.finish_play(t);
				st.case_running = false;
				++st.next_case;
			}
			break;
		}
	}
	return st.next_case >= st.cases.size();
}

int M2bCheck::exit_code() const {
	for (const M2bCaseResult &r : state_->results) {
		if (r.outcome == M2aOutcome::Fail) {
			return 1;
		}
	}
	return state_->results.size() == state_->cases.size() ? 0 : 1;
}

std::vector<M2bCaseResult> M2bCheck::results() const {
	return state_->results;
}

std::string M2bCheck::report() const {
	std::string out;
	uint32_t pass = 0, fail = 0, skip = 0;
	for (const M2bCaseResult &r : state_->results) {
		const std::string c(m2b_case_name(r.which));
		if (r.which == M2bCase::Play) {
			for (const M2bPlayRow &row : state_->play_rows) {
				const char *verdict = row.state == M2bRowState::Pass ? "PASS" : (row.state == M2bRowState::Fail ? "FAIL" : "SKIP");
				out += fmt("%s %s.%s %.6g %s%s%s%s\n", verdict, c.c_str(), row.name.c_str(), row.value,
						row.rule.c_str(), row.skip_reason.empty() ? "" : " (", row.skip_reason.c_str(),
						row.skip_reason.empty() ? "" : ")");
			}
		} else {
			for (const M2aRow &row : r.rows) {
				out += fmt("%s %s.%s %.6g %s\n", row.pass ? "PASS" : "FAIL", c.c_str(), row.name.c_str(), row.value,
						row.rule.c_str());
			}
		}
		if (r.outcome == M2aOutcome::Skip) {
			++skip;
		} else {
			(r.outcome == M2aOutcome::Pass ? pass : fail) += 1;
		}
	}
	out += fmt("M2B CHECK: %s (%u pass, %u fail, %u skip of %zu cases)",
			fail == 0 && pass + skip == state_->cases.size() ? "PASS" : "FAIL", pass, fail, skip, state_->cases.size());
	return out;
}

}
