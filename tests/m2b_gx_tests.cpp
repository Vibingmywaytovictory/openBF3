// m2b.gx.unit (docs/design/m2b-gx.md §5): the Godot-free rules of the M2b session (m2b_game.cpp) and the R13 change
// in bf3_fx (FxTriggers::sane accepts PresentKind::Revive). No install, no Godot, no session thread, no wall clock.
//   * parse_m2b_options: the play.cmd args, and hostile values (not a number, out of range, trailing text, overflow,
//     NaN) fail with an error naming the arg while the default stays;
//   * the spawn screen's visibility rule and Deploy's enable rule (the server tick against own_respawn_tick);
//   * the SpawnRequest resend cadence (kM2bSpawnRequestRetryTicks) and the reset on a new life;
//   * the play check's rows: gx rows pass and fail on their tally, ui / cam rows skip "waiting for <track>" until the
//     track is built, the lookups row skips without a window;
//   * R13: a Revive event reaches FxTriggers' dispatch (not counted as rejected); an unknown kind still is.

#include <doctest/doctest.h>

#include "m2b_game.h"

#include "bf3/fx/effect_library.h"
#include "bf3/fx/simulator.h"
#include "bf3/fx/triggers.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/presentation_events.h"

#include <array>
#include <cmath>
#include <limits>
#include <string>

using namespace bf3;
using namespace bf3::godot_side;

namespace {

M2bArgText args_with(const char *p_kill, const char *p_pre, const char *p_bots, const char *p_deploy) {
	M2bArgText a;
	if (p_kill != nullptr) {
		a.has_kill_limit = true;
		a.kill_limit = p_kill;
	}
	if (p_pre != nullptr) {
		a.has_preround = true;
		a.preround = p_pre;
	}
	if (p_bots != nullptr) {
		a.has_bots = true;
		a.bots = p_bots;
	}
	if (p_deploy != nullptr) {
		a.has_deploy_screen = true;
		a.deploy_screen = p_deploy;
	}
	return a;
}

const M2bPlayRow *row_named(const std::vector<M2bPlayRow> &p_rows, const char *p_name) {
	for (const M2bPlayRow &r : p_rows) {
		if (r.name == p_name) {
			return &r;
		}
	}
	return nullptr;
}

M2bPlayTally good_tally() {
	M2bPlayTally t;
	t.seconds = 80;
	t.windowed = true;
	t.match_seen = t.in_round_seen = t.join_screen_seen = t.round_over_seen = true;
	t.own_deaths = 2;
	t.deaths_checked = 2;
	t.screens_after_death = 2;
	t.deploys = 3;
	t.deaths_received = 30;
	t.feed_lines = 30;
	t.scoreboard_samples = 5;
	t.kill_cards_checked = 2;
	t.spawn_screen_model_shown = 3;
	t.round_end_shown = true;
	t.death_views = 2;
	return t;
}

} // namespace

TEST_SUITE("m2b-gx") {

TEST_CASE("m2b.gx.unit options: defaults, play.cmd's args, hostile values") {
	M2bOptions o;
	CHECK(parse_m2b_options(M2bArgText{}, o).ok());
	CHECK(o.deploy_screen);
	CHECK(o.kill_limit == 0);
	CHECK(o.preround_s < 0.0f);
	CHECK(o.bots == kM2bPlayableDefaultBots);

	M2bOptions p;
	CHECK(parse_m2b_options(args_with("15", "0", "31", "1"), p).ok());
	CHECK(p.kill_limit == 15);
	CHECK(p.preround_s == 0.0f);
	CHECK(p.bots == 31);
	CHECK(p.deploy_screen);
	M2bOptions q;
	CHECK(parse_m2b_options(args_with(nullptr, "2.5", "0", "0"), q).ok());
	CHECK(q.preround_s == doctest::Approx(2.5));
	CHECK(q.bots == 0);
	CHECK(!q.deploy_screen);
	M2bOptions e;
	CHECK(parse_m2b_options(args_with(nullptr, nullptr, nullptr, ""), e).ok()); // --deploy-screen alone: on
	CHECK(e.deploy_screen);

	// Hostile: every bad value is an error naming its arg; the default stays.
	const std::array<const char *, 9> bad_ints{ "", "x", "5x", " 5", "-1", "100001", "99999999999999999999", "1e3", "0x10" };
	for (const char *v : bad_ints) {
		M2bOptions r;
		const core::Status s = parse_m2b_options(args_with(v, nullptr, nullptr, nullptr), r);
		CHECK_MESSAGE(!s.ok(), v);
		if (!s.ok()) {
			CHECK(s.error().message.find("--kill-limit") != std::string::npos);
		}
		CHECK(r.kill_limit == 0);
	}
	const std::array<const char *, 7> bad_bots{ "32", "-3", "abc", "", "31.0", "4294967296", "1 " };
	for (const char *v : bad_bots) {
		M2bOptions r;
		CHECK_MESSAGE(!parse_m2b_options(args_with(nullptr, nullptr, v, nullptr), r).ok(), v);
		CHECK(r.bots == kM2bPlayableDefaultBots);
	}
	const std::array<const char *, 7> bad_seconds{ "nan", "inf", "-0.5", "601", "", "1e2", "2s" };
	for (const char *v : bad_seconds) {
		M2bOptions r;
		CHECK_MESSAGE(!parse_m2b_options(args_with(nullptr, v, nullptr, nullptr), r).ok(), v);
		CHECK(r.preround_s < 0.0f);
	}
	M2bOptions d;
	CHECK(!parse_m2b_options(args_with(nullptr, nullptr, nullptr, "yes"), d).ok());
	CHECK(d.deploy_screen);
	// The first error is reported; the good values still apply.
	M2bOptions m;
	const core::Status s = parse_m2b_options(args_with("abc", "3", "zz", nullptr), m);
	REQUIRE(!s.ok());
	CHECK(s.error().message.find("--kill-limit") != std::string::npos);
	CHECK(m.preround_s == 3.0f);
	// A very long value is refused, not parsed.
	M2bOptions l;
	CHECK(!parse_m2b_options(args_with(std::string(4096, '9').c_str(), nullptr, nullptr, nullptr), l).ok());
}

TEST_CASE("m2b.gx.unit the spawn screen and Deploy") {
	M2bScreenInput in;
	CHECK(!m2b_spawn_screen_up(in)); // no match yet
	in.match_valid = true;
	in.phase = net::MatchPhase::Loading;
	CHECK(!m2b_spawn_screen_up(in));
	in.phase = net::MatchPhase::PreRound;
	CHECK(m2b_spawn_screen_up(in)); // join: not alive
	in.phase = net::MatchPhase::InRound;
	CHECK(m2b_spawn_screen_up(in));
	in.kill_card_up = true;
	CHECK(!m2b_spawn_screen_up(in)); // the kill card first
	in.kill_card_up = false;
	in.own_alive = true;
	CHECK(!m2b_spawn_screen_up(in));
	in.own_unspawned = true;
	CHECK(m2b_spawn_screen_up(in)); // unspawned by the round restart
	in.phase = net::MatchPhase::RoundOver;
	CHECK(!m2b_spawn_screen_up(in)); // the round end shows instead

	net::MatchState ms;
	ms.phase = net::MatchPhase::InRound;
	CHECK(!m2b_can_deploy(false, ms, 100, 0));
	CHECK(m2b_can_deploy(true, ms, 100, 0)); // join: respawn tick 0
	CHECK(!m2b_can_deploy(true, ms, 100, 101));
	CHECK(m2b_can_deploy(true, ms, 101, 101));
	ms.phase = net::MatchPhase::PreRound;
	CHECK(m2b_can_deploy(true, ms, 5, 0));
	ms.phase = net::MatchPhase::RoundOver;
	CHECK(!m2b_can_deploy(true, ms, 1000, 0));
	ms.phase = net::MatchPhase::Loading;
	CHECK(!m2b_can_deploy(true, ms, 1000, 0));
}

TEST_CASE("m2b.gx.unit the scoreboard at RoundOver and the play script's helpers") {
	CHECK(m2b_scoreboard_shown(true, true, net::MatchPhase::InRound));
	CHECK(!m2b_scoreboard_shown(false, true, net::MatchPhase::InRound));
	CHECK(!m2b_scoreboard_shown(true, true, net::MatchPhase::RoundOver)); // the round end takes precedence
	CHECK(m2b_scoreboard_shown(true, false, net::MatchPhase::RoundOver)); // no match known: Tab as held

	// The screen window ends at RoundOver: a death 2 s before it never had the 8 s, however long the run goes on.
	CHECK(m2b_death_had_screen_time(10.0, 18.0, -1.0, 8.0));
	CHECK(!m2b_death_had_screen_time(10.0, 17.9, -1.0, 8.0));
	CHECK(!m2b_death_had_screen_time(30.0, 60.0, 32.0, 8.0));
	CHECK(m2b_death_had_screen_time(10.0, 60.0, 32.0, 8.0));

	// In front of the enemy along its view (forward = (sin yaw, cos yaw)), facing back at it.
	const sim::Vec3 enemy{ 10.0f, 2.0f, -4.0f };
	M2bSeekSpot s = m2b_seek_spot(enemy, 0.0f, 6.0f);
	CHECK(s.position[0] == doctest::Approx(10.0f));
	CHECK(s.position[1] == doctest::Approx(2.0f));
	CHECK(s.position[2] == doctest::Approx(2.0f));
	CHECK(std::abs(s.yaw) == doctest::Approx(3.14159265f));
	s = m2b_seek_spot(enemy, 1.5707963f, 6.0f);
	CHECK(s.position[0] == doctest::Approx(16.0f));
	CHECK(s.position[2] == doctest::Approx(-4.0f).epsilon(1e-3));
	CHECK(s.yaw == doctest::Approx(-1.5707963f));
	// facing it: the direction from the spot to the enemy is the spot's forward
	CHECK(std::sin(s.yaw) == doctest::Approx(-1.0f));
	s = m2b_seek_spot(enemy, std::nanf(""), 6.0f);
	CHECK(s.position == enemy);
	CHECK(s.yaw == 0.0f);
}

TEST_CASE("m2b.gx.unit the SpawnRequest and its resend") {
	M2bDeploy d;
	CHECK(!m2b_deploy_step(d, true, true, 10)); // not armed
	d.armed = true;
	d.kit = 2;
	CHECK(!m2b_deploy_step(d, false, true, 10)); // the screen is not up
	CHECK(!m2b_deploy_step(d, true, false, 10)); // Deploy not enabled yet
	CHECK(m2b_deploy_step(d, true, true, 10));
	CHECK(d.requests == 1);
	for (sim::Tick t = 11; t < 10 + kM2bSpawnRequestRetryTicks; ++t) {
		CHECK(!m2b_deploy_step(d, true, true, t));
	}
	CHECK(m2b_deploy_step(d, true, true, 10 + kM2bSpawnRequestRetryTicks));
	CHECK(d.requests == 2);
	CHECK(d.resends == 1);
	// A server tick that went backwards (a new client) resends at once instead of waiting 2^32 ticks.
	CHECK(m2b_deploy_step(d, true, true, 3));
	CHECK(d.resends == 2);
	m2b_deploy_spawned(d);
	CHECK(!d.armed);
	CHECK(!d.sent);
	CHECK(!m2b_deploy_step(d, true, true, 500));
	CHECK(d.requests == 3);
}

TEST_CASE("m2b.gx.unit the play rows: gx verdicts, ui / cam skipped until built") {
	const M2bPlayTally t = good_tally();
	{
		const std::vector<M2bPlayRow> rows = m2b_play_rows(t, true, true);
		for (const M2bPlayRow &r : rows) {
			CHECK_MESSAGE(r.state == M2bRowState::Pass, r.name);
		}
		CHECK(rows.size() == 14);
	}
	{
		const std::vector<M2bPlayRow> rows = m2b_play_rows(t, false, false);
		size_t gx = 0, skipped = 0;
		for (const M2bPlayRow &r : rows) {
			if (r.track == M2bTrack::Gx) {
				++gx;
				CHECK(r.state == M2bRowState::Pass);
			} else {
				++skipped;
				CHECK(r.state == M2bRowState::Skip);
				CHECK(r.skip_reason == (r.track == M2bTrack::Ui ? "waiting for ui" : "waiting for cam"));
			}
		}
		CHECK(gx == 7);
		CHECK(skipped == 7);
	}
	{
		M2bPlayTally h = t;
		h.windowed = false;
		const std::vector<M2bPlayRow> rows = m2b_play_rows(h, true, true);
		const M2bPlayRow *r = row_named(rows, "missing_lookups");
		REQUIRE(r != nullptr);
		CHECK(r->state == M2bRowState::Skip);
		CHECK(r->skip_reason == "headless: nothing is drawn");
	}
	// Each gx rule fails on its own.
	const auto fails = [&](M2bPlayTally p_t, const char *p_row) {
		const std::vector<M2bPlayRow> rows = m2b_play_rows(p_t, false, false);
		const M2bPlayRow *r = row_named(rows, p_row);
		REQUIRE(r != nullptr);
		CHECK_MESSAGE(r->state == M2bRowState::Fail, p_row);
	};
	M2bPlayTally x = t;
	x.in_round_seen = false;
	fails(x, "match_state");
	x = t;
	x.join_screen_seen = false;
	fails(x, "spawn_screen_join");
	x = t;
	x.own_deaths = 0;
	fails(x, "own_deaths");
	x = t;
	x.screens_after_death = 1;
	fails(x, "spawn_screen_after_death");
	x = t;
	x.deaths_checked = 0;
	x.screens_after_death = 0;
	fails(x, "spawn_screen_after_death"); // no death with time to show it proves nothing
	x = t;
	x.deploys = 1;
	fails(x, "deploys");
	x = t;
	x.deploy_kit_mismatch = 1;
	fails(x, "deploy_kit_mismatch");
	x = t;
	x.round_over_seen = false;
	fails(x, "round_over");
	// ui and cam fail on their tallies once built.
	x = t;
	x.feed_lines = 29;
	CHECK(row_named(m2b_play_rows(x, true, true), "kill_feed_lines")->state == M2bRowState::Fail);
	x = t;
	x.scoreboard_mismatch = 1;
	CHECK(row_named(m2b_play_rows(x, true, true), "scoreboard_kills_mismatch")->state == M2bRowState::Fail);
	x = t;
	x.death_view_bad_inputs = 1;
	CHECK(row_named(m2b_play_rows(x, true, true), "death_views")->state == M2bRowState::Fail);
}

TEST_CASE("m2b.gx.unit R13: FxTriggers accepts a Revive event") {
	fx::FxWorld world;
	fx::EffectLibrary library{ fx::EffectLibrary::Sources{} };
	fx::FxTriggers triggers(world, library, nullptr, {});
	std::array<host::PresentEvent, 3> events{};
	events[0].kind = host::PresentKind::Revive;
	events[0].timeline = host::PresentTimeline::Immediate;
	events[0].slot = 3;
	events[0].aux = 7;
	events[1].kind = host::PresentKind::Spawn;
	events[1].timeline = host::PresentTimeline::Immediate;
	events[2].kind = host::PresentKind(uint8_t(host::PresentKind::Revive) + 1); // hostile: an unknown kind
	events[2].timeline = host::PresentTimeline::Immediate;
	host::PresentClock clock;
	triggers.on_events(events, clock, fx::FxView{});
	CHECK(triggers.stats().events == 3);
	CHECK(triggers.stats().rejected == 1); // only the unknown kind
}

} // TEST_SUITE
