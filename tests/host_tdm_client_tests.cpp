#include <doctest/doctest.h>

#include "gunplay_fixture.h"
#include "tdm_fixture.h"

#include "bf3/host/tdm_brain.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

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
			{
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

			std::optional<sim::Vec3> drop;
			{
				core::Result<std::unique_ptr<sim::CollisionWorld>> w = sim::CollisionWorld::create(level->shared->static_world);
				REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
				sim::SoldierMotor motor;
				const core::Status mi = motor.init(**w, level->shared->motor);
				REQUIRE_MESSAGE(mi.ok(), (mi.ok() ? std::string() : mi.error().message));
				drop = sim::find_drop_spot(motor, **w);
			}
			REQUIRE(drop.has_value());
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

				bots->count = 31;
				r_cfg.tdm_bots = bots;
			};
			const auto start = std::chrono::steady_clock::now();
			std::unique_ptr<tg::Session> s = tg::start_session(level->shared, so);
			if (s == nullptr) {
				return;
			}

			tg::run_for(*s, 1.0);
			CHECK(s->frame->own_curr.life == 0u);
			CHECK(s->frame->match_valid);
			const host::ClientStats before = s->client.stats();

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

			const std::optional<net::DebugReport> moved = tg::teleport(*s, { (*drop)[0], (*drop)[1] + 46.0f, (*drop)[2] }, s->frame->own_curr.yaw);
			REQUIRE(moved.has_value());
			CHECK(moved->ok);
			tg::run_for(*s, 5.0);
			std::printf("E13 drop at (%.1f, %.1f, %.1f) + 46 m: life %u, health %.1f, alive %d\n", double((*drop)[0]), double((*drop)[1]),
					double((*drop)[2]), unsigned(s->frame->own_curr.life), double(s->frame->own_curr.health),
					int((s->frame->own_curr.flags & sim::soldier_flags::kAlive) != 0));
			std::printf("E13 death: own_respawn_tick %u, own_revive_until %u, server tick ~%u\n", unsigned(s->frame->own_respawn_tick),
					unsigned(s->frame->own_revive_until), unsigned(s->frame->match.round_start_tick));
			CHECK(s->frame->own_respawn_tick != 0u);
			CHECK(s->frame->own_revive_until != 0u);

			const auto round_start = std::chrono::steady_clock::now();
			while (tg::seconds_since(round_start) < 30.0 && s->frame->match.phase != net::MatchPhase::RoundOver) {
				tg::run_for(*s, 0.25);
				if (s->frame->own_curr.life != 0 && (s->frame->own_curr.flags & sim::soldier_flags::kAlive) == 0 &&
						s->frame->own_respawn_tick != 0) {
					s->client.send_spawn_request(0);
				}
			}
			std::printf("E13: %s after %.1f s (total %.1f s), kills %d / %d of %d, winner %u\n",
					s->frame->match.phase == net::MatchPhase::RoundOver ? "RoundOver" : "no RoundOver", tg::seconds_since(round_start), tg::seconds_since(start),
					int(s->frame->match.kills[0]), int(s->frame->match.kills[1]), int(s->frame->match.kill_limit), unsigned(s->frame->match.winner));
			CHECK(s->frame->match.phase == net::MatchPhase::RoundOver);
			CHECK(tg::seconds_since(start) <= 60.0);
		}
	}
}
