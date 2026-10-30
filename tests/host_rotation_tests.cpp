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

std::shared_ptr<const host::SharedLevel> renamed(
		const std::shared_ptr<const host::SharedLevel> &p_level, const std::string &p_name) {
	auto copy = std::make_shared<host::SharedLevel>(*p_level);
	copy->level = p_name;
	return copy;
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

	TEST_CASE(
			"level change: the seats keep their slot ids, names and teams through two swaps; a Hello during the change "
			"gets the next level's MatchInfo in phase LevelChange and is held; a Ready then is ignored; each carried "
			"seat loads on the next server until its Ready") {
		const std::shared_ptr<const host::SharedLevel> one = tg::synthetic_floor();
		REQUIRE(one != nullptr);
		const std::array<std::shared_ptr<const host::SharedLevel>, 3> levels{ one, renamed(one, "SyntheticTwo"),
			renamed(one, "SyntheticThree") };
		auto server = std::make_unique<host::GameServer>();
		host::GameServerConfig c;
		c.level = levels[0];
		c.max_slots = 8;
		REQUIRE(server->init(c).ok());
		std::vector<std::unique_ptr<tt::ScriptedPeer>> peers;
		for (const char *name : { "alpha", "gone", "bravo" }) {
			peers.push_back(std::make_unique<tt::ScriptedPeer>(*server, uint32_t(peers.size() + 1u), name));
			peers.back()->hello();
			peers.back()->ready();
		}
		std::vector<host::OutDatagram> out;
		const auto tick = [&] {
			out.clear();
			server->tick(out);
			for (auto &p : peers) {
				p->receive(out);
			}
		};
		tick();
		server->on_receive(peers[1]->peer(), net::kChannelReliable, net::encode_bye());
		peers.erase(peers.begin() + 1);
		tick();
		REQUIRE(peers[0]->slot() == 0);
		REQUIRE(peers[1]->slot() == 2);
		for (uint32_t level = 1; level < 3; ++level) {
			CAPTURE(level);
			net::LevelChange change;
			change.level = levels[level]->level;
			change.mode = levels[level]->mode;
			change.round_id = level;
			change.load_deadline_s = 120;
			net::NextLevel next;
			next.level = change.level;
			next.mode = change.mode;
			next.source_digest = levels[level]->source_digest;
			server->begin_level_change(change, next);
			CHECK(server->level_changing());
			tick();
			for (auto &p : peers) {
				REQUIRE_FALSE(p->level_changes.empty());
				CHECK(p->level_changes.back().level == change.level);
				CHECK(p->level_changes.back().round_id == level);
			}
			if (level == 1) {
				peers.push_back(std::make_unique<tt::ScriptedPeer>(*server, 9u, "held"));
				peers.back()->hello();
				tick();
				tt::ScriptedPeer &held = *peers.back();
				REQUIRE(held.match_info.has_value());
				CHECK(held.match_info->phase == net::MatchPhase::LevelChange);
				CHECK(held.match_info->level == change.level);
				CHECK(held.match_info->round_id == level);
				CHECK(held.slot() == 1);
				REQUIRE_FALSE(held.level_changes.empty());
				held.ready();
				CHECK(server->stats().held_readies == 1u);
				CHECK(server->stats().held_joins == 1u);
			}
			const std::vector<host::CarriedSeat> seats = server->seats_to_carry();
			host::GameServerConfig n;
			n.level = levels[level];
			n.max_slots = 8;
			n.round_id = level;
			n.carried_seats = server->release_seats();
			CHECK(n.carried_seats.size() == seats.size());
			CHECK(server->stats().clients == 0u);
			auto swapped = std::make_unique<host::GameServer>();
			const core::Status st = swapped->init(n);
			REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
			server = std::move(swapped);
			for (auto &p : peers) {
				p->rebind(*server);
			}
			tick();
			const host::GameServerStats g = server->stats();
			CHECK(g.carried_seats == seats.size());
			CHECK(g.clients == seats.size());
			for (const host::CarriedSeat &s : seats) {
				CAPTURE(s.name);
				const host::SlotStats &slot = g.slots[s.slot];
				CHECK(slot.used);
				CHECK(slot.remote);
				CHECK(slot.peer == s.peer);
				CHECK(slot.load.loading);
			}
			for (auto &p : peers) {
				CAPTURE(p->peer());
				REQUIRE(p->match_info.has_value());
				CHECK(p->match_info->level == levels[level]->level);
				CHECK(p->match_info->phase != net::MatchPhase::LevelChange);
				CHECK(p->match_info->round_id == level);
				p->loading(loading(level, 500));
				p->ready();
			}
			tick();
			const host::GameServerStats r = server->stats();
			for (auto &p : peers) {
				CHECK_FALSE(slot_stats(r, *p).load.loading);
				CHECK(slot_stats(r, *p).load.reports == 1u);
				CHECK_FALSE(p->kick.has_value());
			}
			const std::vector<host::CarriedSeat> after = server->seats_to_carry();
			REQUIRE(after.size() == seats.size());
			for (size_t i = 0; i < seats.size(); ++i) {
				CHECK(after[i].slot == seats[i].slot);
				CHECK(after[i].peer == seats[i].peer);
				CHECK(after[i].name == seats[i].name);
				CHECK(after[i].team == seats[i].team);
			}
		}
		CHECK(peers[0]->slot() == 0);
		CHECK(peers[1]->slot() == 2);
		CHECK(peers[2]->slot() == 1);
		size_t kicks = 0;
		for (const uint64_t k : server->stats().kicks) {
			kicks += size_t(k);
		}
		CHECK(kicks == 0u);
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
