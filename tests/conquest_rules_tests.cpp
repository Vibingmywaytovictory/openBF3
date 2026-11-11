#include <doctest/doctest.h>

#include "bf3/sim/logic/conquest_rules.h"

#include <array>
#include <cmath>
#include <vector>

using namespace bf3::sim;
using namespace bf3::sim::logic;

namespace {

constexpr float kDt = 1.0f / 30.0f;

CapturePointParams conquest_flag() {
	CapturePointParams p;
	p.time_to_get_control = 20;
	p.time_to_lose_control = 20;
	p.added_multiplier_per_player = 0.34f;
	p.max_capture_multiplier = 4;
	p.min_nr_to_take_control = 1;
	p.return_multiplier = 1.0f;
	p.seesaw = true;
	p.start_at_bottom = true;
	p.area_value = 34.0f;
	return p;
}

std::array<uint32_t, kConquestTeams> inside(uint32_t p_us, uint32_t p_ru) {
	return { 0u, p_us, p_ru };
}

struct Run {
	int updates = 0;
	int captured_at = -1;
	int lost_at = -1;
};

Run run_until(const CapturePointParams &p_params, CapturePointState &r_state, uint32_t p_us, uint32_t p_ru, int p_max,
		bool p_stop_on_capture) {
	Run r;
	const std::array<uint32_t, kConquestTeams> in = inside(p_us, p_ru);
	for (int i = 0; i < p_max; ++i) {
		const CaptureEvents ev =
				step_capture_point(p_params, r_state, std::span<const uint32_t, kConquestTeams>(in), kDt);
		++r.updates;
		if (ev.lost && r.lost_at < 0) {
			r.lost_at = r.updates;
		}
		if (ev.captured) {
			r.captured_at = r.updates;
			if (p_stop_on_capture) {
				break;
			}
		}
	}
	return r;
}

double seconds_to_capture(uint32_t p_players) {
	const CapturePointParams p = conquest_flag();
	CapturePointState s = reset_capture_point(p);
	const Run r = run_until(p, s, p_players, 0, 4000, true);
	return double(r.captured_at - 1) * double(kDt);
}

}

TEST_SUITE("logic-conquest-unit") {
	TEST_CASE(
			"conquest M-CQ1: reset puts a Conquest flag neutral at the bottom, an HQ owned and controlled at the top") {
		const CapturePointParams flag = conquest_flag();
		const CapturePointState f = reset_capture_point(flag);
		CHECK(f.team == 0);
		CHECK(f.progress == 0.0f);
		CHECK(f.home == 0.0f);
		CHECK_FALSE(f.controlled);
		CapturePointParams hq = conquest_flag();
		hq.type = CapturableType::UnableToChangeTeam;
		hq.start_at_bottom = false;
		hq.initial_owner = 1;
		hq.area_value = 0.0f;
		const CapturePointState h = reset_capture_point(hq);
		CHECK(h.team == 1);
		CHECK(h.controller == 1);
		CHECK(h.progress == kFlagTop);
		CHECK(h.controlled);
		CapturePointState moved = h;
		const std::array<uint32_t, kConquestTeams> enemies = inside(0, 6);
		const CaptureEvents ev = step_capture_point(hq, moved, std::span<const uint32_t, kConquestTeams>(enemies), kDt);
		CHECK_FALSE(ev.lost);
		CHECK(moved.progress == kFlagTop);
		CHECK(moved.team == 1);
	}

	TEST_CASE("conquest M-CQ1: one US soldier takes the neutral flag's team in one update, then raises it in 20 s") {
		const CapturePointParams p = conquest_flag();
		CapturePointState s = reset_capture_point(p);
		const std::array<uint32_t, kConquestTeams> one = inside(1, 0);
		const CaptureEvents first = step_capture_point(p, s, std::span<const uint32_t, kConquestTeams>(one), kDt);
		CHECK(s.team == 1);
		CHECK(s.progress == 0.0f);
		CHECK(first.disable_spawners);
		CHECK_FALSE(first.captured);
		const Run r = run_until(p, s, 1, 0, 2000, true);
		CHECK(r.captured_at >= 600);
		CHECK(r.captured_at <= 601);
		CHECK(s.controlled);
		CHECK(s.controller == 1);
		CHECK(s.home == kFlagTop);
		CHECK(s.rate == 0.0f);
	}

	TEST_CASE("conquest M-CQ1: the head count multiplier is 1 + 0.34 (n - 1), capped at four players") {
		CHECK(seconds_to_capture(2) == doctest::Approx(1.0 / 30.0 + 100.0 / 6.7).epsilon(0.004));
		CHECK(seconds_to_capture(3) == doctest::Approx(1.0 / 30.0 + 100.0 / 8.4).epsilon(0.004));
		CHECK(seconds_to_capture(4) == doctest::Approx(1.0 / 30.0 + 100.0 / 10.1).epsilon(0.004));
		CHECK(seconds_to_capture(9) == seconds_to_capture(4));
	}

	TEST_CASE("conquest M-CQ1: 3 RU and 1 US on a US flag lower it in 14.93 s, switch it to RU, then raise it for RU") {
		const CapturePointParams p = conquest_flag();
		CapturePointState s = reset_capture_point(p);
		s.team = 1;
		s.controller = 1;
		s.progress = kFlagTop;
		s.home = kFlagTop;
		s.controlled = true;
		const std::array<uint32_t, kConquestTeams> mixed = inside(1, 3);
		const CaptureEvents attacked = step_capture_point(p, s, std::span<const uint32_t, kConquestTeams>(mixed), kDt);
		CHECK(s.attacker == 2);
		CHECK(s.under_attack);
		CHECK(s.rate == doctest::Approx(-6.7f));
		CHECK_FALSE(attacked.lost);
		int down = 1;
		bool lost = false;
		for (; down < 2000 && !lost; ++down) {
			const CaptureEvents ev = step_capture_point(p, s, std::span<const uint32_t, kConquestTeams>(mixed), kDt);
			lost = ev.lost;
			CHECK_FALSE(ev.captured);
		}
		REQUIRE(lost);
		CHECK(double(down) * kDt == doctest::Approx(14.93).epsilon(0.004));
		CHECK(s.controller == 1);
		CHECK_FALSE(s.controlled);
		CHECK(s.team == 2);
		CHECK(s.progress == 0.0f);
		const Run up = run_until(p, s, 1, 3, 2000, true);
		CHECK(double(down + up.captured_at) * kDt == doctest::Approx(29.85).epsilon(0.004));
		CHECK(s.controller == 2);
	}

	TEST_CASE(
			"conquest M-CQ1: equal numbers freeze the pole; an emptied half-raised flag slides home and goes neutral") {
		const CapturePointParams p = conquest_flag();
		CapturePointState s = reset_capture_point(p);
		s.team = 1;
		s.progress = 40.0f;
		const float frozen = s.progress;
		run_until(p, s, 2, 2, 90, false);
		CHECK(s.progress == frozen);
		CHECK(s.rate == 0.0f);
		const Run back = run_until(p, s, 0, 0, 400, false);
		CHECK(s.team == 0);
		CHECK(s.progress == 0.0f);
		CHECK(back.captured_at < 0);
		CapturePointState t = reset_capture_point(p);
		t.team = 1;
		t.progress = 40.0f;
		int steps = 0;
		const std::array<uint32_t, kConquestTeams> none = inside(0, 0);
		while (t.team != 0 && steps < 1000) {
			step_capture_point(p, t, std::span<const uint32_t, kConquestTeams>(none), kDt);
			++steps;
		}
		CHECK(double(steps) * kDt == doctest::Approx(8.0).epsilon(0.01));
	}

	TEST_CASE("conquest M-CQ1: a lowered US flag returns to the top in 10 s without a second OnCaptured") {
		const CapturePointParams p = conquest_flag();
		CapturePointState s = reset_capture_point(p);
		s.team = 1;
		s.controller = 1;
		s.controlled = true;
		s.home = kFlagTop;
		s.progress = 50.0f;
		const Run r = run_until(p, s, 0, 0, 299, false);
		CHECK(s.progress < kFlagTop);
		const Run last = run_until(p, s, 0, 0, 3, false);
		CHECK(s.progress == kFlagTop);
		CHECK(r.captured_at < 0);
		CHECK(last.captured_at < 0);
	}

	TEST_CASE("conquest M-CQ1: Domination flags take 2 s neutral and 8 s down plus 2 s up") {
		CapturePointParams p = conquest_flag();
		p.time_to_get_control = 2;
		p.time_to_lose_control = 8;
		CapturePointState s = reset_capture_point(p);
		const Run neutral = run_until(p, s, 1, 0, 400, true);
		CHECK(double(neutral.captured_at) * kDt == doctest::Approx(2.0 + kDt).epsilon(0.02));
		const Run turned = run_until(p, s, 0, 1, 1000, true);
		CHECK(double(turned.captured_at) * kDt == doctest::Approx(10.0).epsilon(0.01));
		CHECK(s.team == 2);
	}

	TEST_CASE(
			"conquest M-CQ1: the capture volume test is the polygon when a volume is linked, else the strict sphere") {
		CapturePointParams p = conquest_flag();
		p.position = { 10.0f, 5.0f, 10.0f };
		p.capture_radius = 4.0f;
		p.upper_sphere = true;
		CHECK(capture_point_contains(p, { 10.0f, 6.0f, 12.0f }));
		CHECK_FALSE(capture_point_contains(p, { 10.0f, 4.0f, 10.0f }));
		CHECK_FALSE(capture_point_contains(p, { 14.0f, 5.0f, 10.0f }));
		p.volumes.push_back(CaptureVolume{ { { 0.0f, 0.0f }, { 20.0f, 0.0f }, { 20.0f, 20.0f }, { 0.0f, 20.0f } } });
		CHECK(capture_point_contains(p, { 18.0f, -50.0f, 1.0f }));
		CHECK_FALSE(capture_point_contains(p, { 21.0f, 5.0f, 10.0f }));
	}

	TEST_CASE("conquest M-CQ1: the intruder warning fires on the edge from no attacker to an attacker") {
		CapturePointParams p = conquest_flag();
		p.intruder_warning = true;
		CapturePointState s = reset_capture_point(p);
		s.team = 1;
		s.controlled = true;
		s.progress = kFlagTop;
		s.home = kFlagTop;
		const std::array<uint32_t, kConquestTeams> one = inside(0, 1);
		CHECK(step_capture_point(p, s, std::span<const uint32_t, kConquestTeams>(one), kDt).intruder);
		CHECK_FALSE(step_capture_point(p, s, std::span<const uint32_t, kConquestTeams>(one), kDt).intruder);
	}

	TEST_CASE("conquest M-CQ3/M-CQ4: team-locked spawners follow the flag's team, unlocked ones take it") {
		SpawnerGate us{ 1, true, 1, false };
		SpawnerGate ru{ 2, true, 2, false };
		SpawnerGate vehicle{ 2, false, 2, true };
		enable_spawner(us, 0);
		enable_spawner(ru, 0);
		enable_spawner(vehicle, 0);
		CHECK_FALSE(us.enabled);
		CHECK_FALSE(ru.enabled);
		CHECK(vehicle.team == 0);
		enable_spawner(us, 1);
		enable_spawner(ru, 1);
		enable_spawner(vehicle, 1);
		CHECK(us.enabled);
		CHECK_FALSE(ru.enabled);
		CHECK(vehicle.team == 1);
		disable_spawner(us);
		disable_spawner(vehicle);
		CHECK_FALSE(us.enabled);
		CHECK(vehicle.team == 0);
	}

	TEST_CASE("conquest M-CQ2: initial tickets truncate InitialTicketCount x GameModeCounterModifier into 1..65535") {
		CHECK(initial_tickets(150, 1.0) == 150.0);
		CHECK(initial_tickets(150, 2.0) == 300.0);
		CHECK(initial_tickets(150, 0.333) == 49.0);
		CHECK(initial_tickets(150, 0.0) == 1.0);
		CHECK(initial_tickets(400, 1000.0) == 65535.0);
	}

	TEST_CASE("conquest M-CQ2: the first spawn is free, a redeploy costs one, a revive costs nothing, loss waits for "
			  "StartRound") {
		TicketCounterParams p;
		p.team = 1;
		p.initial_ticket_count = 150;
		p.decrease = TicketDecrease::OnRespawned;
		TicketCounterState s = reset_ticket_counter(p, 1.0);
		ticket_respawn(p, s, 3, 1);
		ticket_respawn(p, s, 3, 1);
		CHECK(s.displayed() == 150);
		s.round_started = true;
		ticket_respawn(p, s, 4, 1);
		CHECK(s.displayed() == 150);
		ticket_killed(p, s, 1, false);
		CHECK(s.displayed() == 150);
		ticket_respawn(p, s, 4, 1);
		CHECK(s.displayed() == 149);
		ticket_respawn(p, s, 9, 2);
		CHECK(s.displayed() == 149);
		ticket_left(p, s, 4, 1);
		ticket_respawn(p, s, 4, 1);
		CHECK(s.displayed() == 149);
	}

	TEST_CASE("conquest M-CQ2: bleed on MP_007 CQS is 0, 0, 0.255 and 0.34 tickets/s for 2, 2-1, 3 and 4 enemy flags") {
		TicketCounterParams p;
		p.team = 1;
		p.initial_ticket_count = 150;
		p.loss_per_min = 15.0f;
		p.loss_start = 100.0f;
		p.loss_when_lost_all = 999.0f;
		p.loss_when_lost_all_controls_point = 999.0f;
		p.halt_on_equal_point_count = true;
		const auto rate = [&](double p_mine, double p_enemy, bool p_all) {
			TicketCounterState s = reset_ticket_counter(p, 1.0);
			s.round_started = true;
			s.respawn_seen = true;
			s.area = p_mine;
			BleedInputs in;
			in.total_area = p_mine + p_enemy;
			in.holds_point = true;
			in.soldier_alive = true;
			in.enemy_holds_all = p_all;
			const TicketEvents ev = ticket_bleed(p, s, in, 1.0);
			CHECK(ev.all_held == p_all);
			return 150.0 - s.tickets;
		};
		CHECK(rate(68, 68, false) == 0.0);
		CHECK(rate(34, 68, false) == 0.0);
		CHECK(rate(0, 102, false) == doctest::Approx(0.255));
		CHECK(rate(0, 136, true) == doctest::Approx(0.34));
		CHECK(rate(0, 170, true) == doctest::Approx(0.425));
	}

	TEST_CASE("conquest M-CQ2: Domination 0.30 and 0.45; CQA large 0.3125 only from 5 flags") {
		TicketCounterParams p;
		p.team = 2;
		p.initial_ticket_count = 150;
		p.loss_per_min = 15.0f;
		p.loss_start = 100.0f;
		p.halt_on_equal_point_count = true;
		const auto rate = [&](double p_mine, double p_enemy) {
			TicketCounterState s = reset_ticket_counter(p, 1.0);
			s.round_started = true;
			s.respawn_seen = true;
			s.area = p_mine;
			BleedInputs in;
			in.total_area = p_mine + p_enemy;
			in.holds_point = true;
			in.soldier_alive = true;
			ticket_bleed(p, s, in, 1.0);
			return 150.0 - s.tickets;
		};
		CHECK(rate(60, 120) == doctest::Approx(0.30));
		CHECK(rate(0, 180) == doctest::Approx(0.45));
		CHECK(rate(50, 100) == 0.0);
		CHECK(rate(0, 125) == doctest::Approx(0.3125));
		CHECK(rate(25, 175) == doctest::Approx(0.4375));
	}

	TEST_CASE("conquest M-CQ6: CQA small, RU holds no flag: 0.75/s while a soldier lives, 16.65/s once none does") {
		TicketCounterParams p;
		p.team = 2;
		p.initial_ticket_count = 250;
		p.loss_per_min = 15.0f;
		p.loss_start = 100.0f;
		p.loss_when_lost_all = 999.0f;
		p.loss_when_lost_all_controls_point = 45.0f;
		p.halt_on_equal_point_count = true;
		p.spawn_always_allowed = true;
		TicketCounterState s = reset_ticket_counter(p, 1.0);
		s.round_started = true;
		s.respawn_seen = true;
		BleedInputs in;
		in.total_area = 175.0;
		in.holds_point = false;
		in.soldier_alive = true;
		ticket_bleed(p, s, in, 1.0);
		CHECK(250.0 - s.tickets == doctest::Approx(0.75));
		in.soldier_alive = false;
		const double before = s.tickets;
		ticket_bleed(p, s, in, 1.0);
		CHECK(before - s.tickets == doctest::Approx(16.65));
	}

	TEST_CASE("conquest M-CQ2: area bookkeeping uses the flag's team on capture and its controller on loss; tickets "
			  "stop at 0") {
		TicketCounterParams us;
		us.team = 1;
		us.initial_ticket_count = 2;
		us.loss_per_min = 15.0f;
		us.loss_start = 100.0f;
		CapturePointParams flag = conquest_flag();
		CapturePointState st = reset_capture_point(flag);
		TicketCounterState s = reset_ticket_counter(us, 1.0);
		st.team = 1;
		st.controller = 1;
		ticket_captured(us, s, flag, st);
		CHECK(s.area == 34.0);
		st.team = 2;
		ticket_lost(us, s, flag, st);
		CHECK(s.area == 0.0);
		s.round_started = true;
		s.respawn_seen = true;
		BleedInputs in;
		in.total_area = 136.0;
		in.holds_point = true;
		in.soldier_alive = true;
		for (int i = 0; i < 1000; ++i) {
			ticket_bleed(us, s, in, 1.0);
		}
		CHECK(s.tickets == 0.0);
		CHECK(s.stopped);
		CHECK(s.displayed() == 0);
		CHECK_FALSE(s.losing());
	}

	TEST_CASE("conquest M-CQ3: a team may spawn while it holds a point (HQ included) or when SpawnAlwaysAllowed") {
		CapturePointParams hq = conquest_flag();
		hq.type = CapturableType::UnableToChangeTeam;
		hq.start_at_bottom = false;
		hq.initial_owner = 2;
		const CapturePointState hq_state = reset_capture_point(hq);
		const CapturePointParams flag = conquest_flag();
		const CapturePointState flag_state = reset_capture_point(flag);
		const std::vector<ConquestPoint> points{ { &hq, &hq_state }, { &flag, &flag_state } };
		TicketCounterParams us;
		us.team = 1;
		TicketCounterParams ru;
		ru.team = 2;
		CHECK_FALSE(spawn_allowed(us, 1, points));
		CHECK(spawn_allowed(us, 2, points));
		CHECK(spawn_allowed(ru, 2, points));
		us.spawn_always_allowed = true;
		CHECK(spawn_allowed(us, 1, points));
		CHECK_FALSE(enemy_holds_all(1, points));
	}

	TEST_CASE("conquest M-CQ5: a deserter is struck at 9 s and killed at 10 s; returning clears the timer") {
		CombatAreaParams area;
		area.team = 1;
		area.team_specific = true;
		area.time_to_return = 10;
		std::vector<Deserter> list;
		std::vector<CombatAreaPlayer> players{ { 5, 1, true, false, false }, { 6, 2, true, false, false } };
		int strike_at = -1, kill_at = -1;
		for (int i = 0; i < 400 && kill_at < 0; ++i) {
			const CombatAreaEvents ev = step_combat_area(area, list, players, false, kDt);
			if (i == 0) {
				REQUIRE(ev.entered.size() == 1);
				CHECK(ev.entered[0].slot == 5);
				CHECK(list.front().timer == doctest::Approx(10.0f - kDt));
			}
			if (!ev.strikes.empty() && strike_at < 0) {
				strike_at = i;
			}
			if (!ev.kills.empty()) {
				kill_at = i;
				CHECK(ev.kills.front() == 5);
			}
		}
		CHECK(double(strike_at + 1) * kDt == doctest::Approx(9.0).epsilon(0.005));
		CHECK(double(kill_at + 1) * kDt == doctest::Approx(10.0).epsilon(0.005));
		CHECK(list.empty());
		for (int i = 0; i < 180; ++i) {
			step_combat_area(area, list, players, false, kDt);
		}
		players[0].inside = true;
		const CombatAreaEvents back = step_combat_area(area, list, players, false, kDt);
		REQUIRE(back.returned.size() == 1);
		CHECK(list.empty());
		players[0].inside = false;
		step_combat_area(area, list, players, false, kDt);
		CHECK(list.front().timer == doctest::Approx(10.0f - kDt));
		CHECK(step_combat_area(area, list, players, true, kDt).kills.empty());
		CombatAreaPlayer bot{ 7, 1, true, true, false };
		CHECK_FALSE(combat_area_tracks(area, bot, false));
		area.ai_allowed_outside = false;
		CHECK(combat_area_tracks(area, bot, false));
	}
}
