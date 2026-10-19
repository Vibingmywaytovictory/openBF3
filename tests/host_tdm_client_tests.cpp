// host.tdm.client (docs/design/m2b.md §11 E13; docs/design/m2b-ru.md §6; owner ru, label timing): the real client path
// on Metro TDM: ServerHost + ClientSession over ENet loopback (tests/gunplay_fixture.h start_session), rules on with the
// deploy screen, 7 TDM bots, kill limit 5, pre-round 3 s. It waits on real time and sockets, so it is a timing row (the
// merge pass, serially, docs/testing.md §1) and is never run on a branch.
//   * no Spawned (no own life) before the first send_spawn_request; Spawned after it
//   * the pre-round freeze predicted by the client: 0 corrections above teleport_above_m and 0 predicted shots while
//     MatchState restricts the input (move and fire held through the freeze)
//   * after a scripted death (a dev Teleport high up) the frame's own_respawn_tick / own_revive_until are set as the
//     server's rules stamped them (respawn = death + 300, revive_until > death)
//   * MatchState reaches RoundOver (kill limit 5) within 30 s
// It needs nt's v3 codecs and client (kProtocolVersion 3, send_spawn_request, the frame's match fields) and bb's
// brains (add_tdm_bots); before both it exits 77 (skipped) with PENDING (nt is in since the w1 merge; the bb probe
// below inits a GameServer with one TDM bot and skips while the W1.0 stub refuses it). The revive half of E13 (a scripted assault bot
// revives the client) is not in this row yet (docs/design/m2b-ru.md §8): host.tdm.server's revive case covers the
// server side and the peer's Spawned.
// Exits 77 (skipped) without an install.

#include <doctest/doctest.h>

#include "gunplay_fixture.h"
#include "tdm_fixture.h"

#include "bf3/host/tdm_brain.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace bf3;
namespace tdm = bf3::test::tdm;
namespace tg = bf3::test::gunplay;

TEST_SUITE("host-tdm-client") {
	TEST_CASE("E13: deploy screen, the predicted freeze, the death stamps, RoundOver") {
		if constexpr (net::kProtocolVersion < 3) {
			std::printf("host.tdm.client: PENDING (nt): the v3 codecs and the client's send_spawn_request / match fields\n");
			bf3::test::skip_install_case("host.tdm.client needs nt's protocol v3");
			return;
		} else {
			tdm::TdmLevel *level = tdm::load_tdm_level_once();
			if (level == nullptr) {
				return;
			}
			{ // bb's add_tdm_bots: until bb lands, the W1.0 stub refuses a config with TDM bots (skipped, PENDING)
				host::GameServer probe;
				host::GameServerConfig c;
				c.level = level->shared;
				c.max_slots = 16;
				c.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
				c.rules.mode = host::MatchRulesConfig::Mode::Tdm;
				auto fill = std::make_shared<host::BotFillConfig>();
				fill->level = level->bots;
				fill->count = 0;
				c.bots = fill;
				auto bots = std::make_shared<host::TdmBotConfig>();
				bots->count = 1;
				c.tdm_bots = bots;
				const core::Status st = probe.init(c);
				if (!st.ok() && st.error().message.find("W1.0 stub") != std::string::npos) {
					std::printf("host.tdm.client: PENDING (bb): %s\n", st.error().message.c_str());
					bf3::test::skip_install_case("host.tdm.client needs bb's add_tdm_bots");
					return;
				}
			}
			tg::SessionOptions so;
			so.max_slots = 64;
			so.client_mechanics_cfg = OPENBF3_MECHANICS_CFG;
			so.configure_server = [&](host::GameServerConfig &r_cfg) {
				r_cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
				r_cfg.rules.mode = host::MatchRulesConfig::Mode::Tdm;
				r_cfg.rules.deploy_screen = true;
				r_cfg.rules.revive = true;
				r_cfg.rules.kill_limit_override = 5;
				r_cfg.rules.preround_override_s = 3.0f;
				auto fill = std::make_shared<host::BotFillConfig>();
				fill->level = level->bots;
				r_cfg.bots = fill;
				auto bots = std::make_shared<host::TdmBotConfig>();
				bots->count = 7;
				r_cfg.tdm_bots = bots;
			};
			const auto start = std::chrono::steady_clock::now();
			std::unique_ptr<tg::Session> s = tg::start_session(level->shared, so);
			if (s == nullptr) {
				return;
			}
			// No life before the request (R10).
			tg::run_for(*s, 1.0);
			CHECK(s->frame->own_curr.life == 0u);
			CHECK(s->frame->match_valid);
			const host::ClientStats before = s->client.stats();
			// The freeze: move and fire held from the first own state on.
			s->client.send_spawn_request(1);
			REQUIRE(tg::wait_spawned(*s, 5.0));
			s->input.move_forward = 1.0f;
			s->input.held_buttons = sim::buttons::kFire;
			uint64_t restricted_shots = 0;
			std::vector<host::PredictedShot> shots;
			std::vector<host::PredictedTimed> timed;
			tg::run_for(*s, 4.0, [&](bool, uint64_t, double) {
				s->client.take_predicted(shots, timed);
				const net::MatchState &ms = s->frame->match;
				for (const host::PredictedShot &p : shots) {
					restricted_shots += ms.restrict_until_tick != 0 && p.tick < ms.restrict_until_tick ? 1u : 0u;
				}
				shots.clear();
				timed.clear();
			});
			s->input.move_forward = 0.0f;
			s->input.held_buttons = 0;
			const host::ClientStats after = s->client.stats();
			std::printf("E13 freeze: %llu predicted shots while restricted, %llu teleport corrections\n", (unsigned long long)restricted_shots,
					(unsigned long long)(after.teleports - before.teleports));
			CHECK(restricted_shots == 0u);
			CHECK(after.teleports == before.teleports);
			// A scripted death: up and down.
			const sim::Vec3 at = s->frame->own_curr.position;
			REQUIRE(tg::teleport(*s, { at[0], at[1] + 60.0f, at[2] }, s->frame->own_curr.yaw).has_value());
			tg::run_for(*s, 5.0);
			std::printf("E13 death: own_respawn_tick %u, own_revive_until %u, server tick ~%u\n", unsigned(s->frame->own_respawn_tick),
					unsigned(s->frame->own_revive_until), unsigned(s->frame->match.round_start_tick));
			CHECK(s->frame->own_respawn_tick != 0u);
			CHECK(s->frame->own_revive_until != 0u);
			// The round ends at the kill limit.
			const auto round_start = std::chrono::steady_clock::now();
			while (tg::seconds_since(round_start) < 30.0 && s->frame->match.phase != net::MatchPhase::RoundOver) {
				tg::run_for(*s, 0.25);
				if (s->frame->own_curr.life != 0 && (s->frame->own_curr.flags & sim::soldier_flags::kAlive) == 0 &&
						s->frame->own_respawn_tick != 0) {
					s->client.send_spawn_request(0);
				}
			}
			std::printf("E13: %s after %.1f s (total %.1f s)\n", s->frame->match.phase == net::MatchPhase::RoundOver ? "RoundOver" : "no RoundOver",
					tg::seconds_since(round_start), tg::seconds_since(start));
			CHECK(s->frame->match.phase == net::MatchPhase::RoundOver);
			CHECK(tg::seconds_since(start) <= 60.0);
		}
	}
}
