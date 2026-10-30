#include <doctest/doctest.h>

#include "bf3/host/bot_fill.h"
#include "bf3/host/game_server.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"

#include "gunplay_fixture.h"
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
namespace tg = bf3::test::gunplay;

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

net::Loading loading(uint32_t p_round, uint16_t p_permille, net::LoadWarmth p_warmth = net::LoadWarmth::Warm) {
	net::Loading l;
	l.round_id = p_round;
	l.stage = 1;
	l.permille = p_permille;
	l.warmth = p_warmth;
	return l;
}

struct Watched {
	std::unique_ptr<tt::ScriptedPeer> peer;
	sim::Tick joined = 0;
	std::optional<sim::Tick> kicked;
};

const host::SlotStats &slot_stats(const host::GameServerStats &p_stats, const tt::ScriptedPeer &p_peer) {
	REQUIRE(p_peer.slot() >= 0);
	REQUIRE(size_t(p_peer.slot()) < p_stats.slots.size());
	return p_stats.slots[size_t(p_peer.slot())];
}

}

TEST_SUITE("host-rotation-unit") {
	TEST_CASE("load deadlines: no progress for 30.0 s kicks LoadStalled; a warm load 120.0 s and a cold one 300.0 s "
			  "after it began kick LoadTimeout; a Ready ends the load; a Loading of another round is stale") {
		const std::shared_ptr<const host::SharedLevel> floor = tg::synthetic_floor();
		REQUIRE(floor != nullptr);
		host::GameServer server;
		host::GameServerConfig c;
		c.level = floor;
		c.max_slots = 8;
		c.round_id = 7;
		REQUIRE(server.init(c).ok());
		std::map<std::string, Watched> peers;
		uint32_t id = 1;
		for (const char *name : { "stalled", "warm", "cold", "ready", "silent" }) {
			Watched &w = peers[name];
			w.peer = std::make_unique<tt::ScriptedPeer>(server, id++, name);
			w.peer->hello();
			w.joined = server.current_tick();
		}
		peers["ready"].peer->ready();
		peers["stalled"].peer->loading(loading(7, 10));
		peers["stalled"].peer->loading(loading(7, 10));
		peers["stalled"].peer->loading(loading(6, 900));
		std::vector<host::OutDatagram> out;
		for (sim::Tick t = 1; t <= 300u * kSecond + 2u; ++t) {
			if (t % kSecond == 0) {
				const uint16_t permille = uint16_t(std::min<sim::Tick>(t / kSecond, 999u));
				if (!peers["warm"].kicked) {
					peers["warm"].peer->loading(loading(7, permille));
				}
				if (!peers["cold"].kicked) {
					peers["cold"].peer->loading(loading(7, permille, net::LoadWarmth::Cold));
				}
			}
			out.clear();
			server.tick(out);
			for (auto &[name, w] : peers) {
				w.peer->receive(out);
				if (w.peer->kick && !w.kicked) {
					w.kicked = server.current_tick();
				}
			}
		}
		const auto seconds = [&](const char *p_name) {
			const Watched &w = peers[p_name];
			return w.kicked ? double(*w.kicked - w.joined) / double(kSecond) : -1.0;
		};
		for (auto &[name, w] : peers) {
			std::printf("load deadlines: %-8s %s after %.2f s%s%s\n", name.c_str(), w.kicked ? "kicked" : "kept",
					w.kicked ? double(*w.kicked - w.joined) / double(kSecond)
							 : double(server.current_tick() - w.joined) / double(kSecond),
					w.peer->kick ? ": " : "", w.peer->kick ? w.peer->kick->reason.c_str() : "");
		}
		REQUIRE(peers["stalled"].peer->kick.has_value());
		CHECK(peers["stalled"].peer->kick->code == net::KickReason::LoadStalled);
		CHECK(seconds("stalled") == doctest::Approx(30.0));
		REQUIRE(peers["warm"].peer->kick.has_value());
		CHECK(peers["warm"].peer->kick->code == net::KickReason::LoadTimeout);
		CHECK(seconds("warm") == doctest::Approx(120.0));
		REQUIRE(peers["cold"].peer->kick.has_value());
		CHECK(peers["cold"].peer->kick->code == net::KickReason::LoadTimeout);
		CHECK(seconds("cold") == doctest::Approx(300.0));
		REQUIRE(peers["silent"].peer->kick.has_value());
		CHECK(peers["silent"].peer->kick->code == net::KickReason::LoadTimeout);
		CHECK(seconds("silent") == doctest::Approx(120.0));
		CHECK_FALSE(peers["ready"].peer->kick.has_value());
		const host::GameServerStats st = server.stats();
		CHECK(st.kicks[size_t(net::KickReason::LoadStalled)] == 1u);
		CHECK(st.kicks[size_t(net::KickReason::LoadTimeout)] == 3u);
		CHECK(st.stale_loading == 1u);
		CHECK(st.clients == 1u);
		CHECK_FALSE(slot_stats(st, *peers["ready"].peer).load.loading);
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
