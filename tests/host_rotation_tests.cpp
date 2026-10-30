#include <doctest/doctest.h>

#include "bf3/host/bot_fill.h"
#include "bf3/host/game_server.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"

#include "tdm_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bf3;
namespace tt = bf3::test::tdm;

namespace {

constexpr uint32_t kRoundTicks = 36000;
constexpr uint32_t kIntermissionTicks = 6000;
constexpr sim::Tick kSecond = sim::kSimHz;

host::GameServerConfig metro_config(
		std::shared_ptr<const host::SharedLevel> p_level, const tt::TdmLevel &p_metro, bool p_hold) {
	host::GameServerConfig c;
	c.level = std::move(p_level);
	c.max_slots = 64;
	c.seed = 5;
	c.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
	c.rules.mode = host::MatchRulesConfig::Mode::Tdm;
	c.rules.deploy_screen = false;
	c.rules.revive = true;
	c.rules.kill_limit_override = 1;
	c.rules.preround_override_s = 1.0f;
	c.rules.max_players = 32;
	c.rules.hold_level_end = p_hold;
	auto fill = std::make_shared<host::BotFillConfig>();
	fill->level = p_metro.bots;
	fill->count = 0;
	c.bots = fill;
	auto bots = std::make_shared<host::TdmBotConfig>();
	bots->count = 12;
	bots->tuning_cfg = OPENBF3_BOT_TUNING_CFG;
	c.tdm_bots = bots;
	return c;
}

template <typename F>
bool step_until(host::GameServer &r_server, F p_done, uint32_t p_max,
		const std::function<void(std::span<const host::OutDatagram>)> &p_out = {}) {
	std::vector<host::OutDatagram> out;
	for (uint32_t i = 0; i < p_max; ++i) {
		if (p_done()) {
			return true;
		}
		out.clear();
		r_server.tick(out);
		if (p_out) {
			p_out(out);
		}
	}
	return p_done();
}

net::MatchPhase phase_of(const host::GameServer &p_server) {
	return p_server.rules()->phase();
}

}

TEST_SUITE("host-rotation") {
	TEST_CASE("level end: Metro's 60 s Delay into LevelControl #34800493 ends the level; held, it waits in LevelChange "
			  "until restart_round; unheld, it restarts in place as before") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		for (const bool hold : { true, false }) {
			CAPTURE(hold);
			host::GameServer server;
			const core::Status st = server.init(metro_config(level->shared, *level, hold));
			REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
			REQUIRE(step_until(server, [&] { return phase_of(server) == net::MatchPhase::InRound; }, 600));
			REQUIRE(step_until(server, [&] { return phase_of(server) == net::MatchPhase::RoundOver; }, kRoundTicks));
			const sim::Tick round_over = server.current_tick();
			const uint64_t restarts0 = server.rules()->stats().restarts;
			REQUIRE(step_until(server, [&] { return server.rules()->stats().level_ends > 0; }, kIntermissionTicks));
			const sim::Tick level_end = server.current_tick();
			std::printf("level end (%s): round over at tick %u, level end at tick %u (%.2f s later), restarts %llu\n",
					hold ? "held" : "unheld", unsigned(round_over), unsigned(level_end),
					double(level_end - round_over) / double(kSecond),
					(unsigned long long)server.rules()->stats().restarts);
			CHECK(level_end - round_over >= 60u * kSecond);
			if (hold) {
				CHECK(server.level_ended());
				CHECK(server.rules()->stats().restarts == restarts0);
				CHECK(phase_of(server) == net::MatchPhase::LevelChange);
				step_until(server, [] { return false; }, 120);
				CHECK(server.level_ended());
				CHECK(phase_of(server) == net::MatchPhase::LevelChange);
				CHECK(server.rules()->stats().restarts == restarts0);
				server.restart_round();
				CHECK_FALSE(server.level_ended());
				CHECK(server.rules()->stats().restarts == restarts0 + 1u);
				CHECK(phase_of(server) == net::MatchPhase::PreRound);
				step_until(server, [] { return false; }, 2);
				CHECK(phase_of(server) == net::MatchPhase::PreRound);
			} else {
				CHECK_FALSE(server.level_ended());
				CHECK(server.rules()->stats().restarts == restarts0 + 1u);
			}
			REQUIRE(step_until(server, [&] { return phase_of(server) == net::MatchPhase::InRound; }, 600));
			CHECK(server.rules()->stats().runtime_faults == 0u);
		}
	}

	TEST_CASE("install unchanged") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		tt::check_install_unchanged(*level);
	}
}
