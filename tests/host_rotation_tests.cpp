#include <doctest/doctest.h>

#include "bf3/host/bot_fill.h"
#include "bf3/host/game_server.h"
#include "bf3/host/map_list.h"
#include "bf3/host/rotation.h"
#include "bf3/host/server_session.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"

#include "gunplay_fixture.h"
#include "tdm_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

struct Rotated {
	std::unique_ptr<host::GameServer> server;
	std::vector<std::unique_ptr<tt::ScriptedPeer>> peers;
	std::vector<host::OutDatagram> out;
	std::vector<net::MatchPhase> phases;
	std::vector<std::string> levels;
	uint32_t next_peer = 1;
	std::map<uint32_t, uint32_t> awaiting_sync;
	std::map<uint32_t, std::vector<std::pair<uint32_t, bool>>> synced_after_ready;

	void readied(const tt::ScriptedPeer &p_peer, uint32_t p_round) { awaiting_sync[p_peer.peer()] = p_round; }

	void note_syncs() {
		for (const auto &[peer, round] : awaiting_sync) {
			const bool sent = std::any_of(out.begin(), out.end(), [&](const host::OutDatagram &d) {
				return d.peer == peer && d.channel == net::kChannelBulk &&
						net::peek_type(d.bytes) == net::MessageType::FullSync;
			});
			synced_after_ready[peer].push_back({ round, sent });
		}
		awaiting_sync.clear();
	}

	tt::ScriptedPeer &join(const std::string &p_name) {
		peers.push_back(std::make_unique<tt::ScriptedPeer>(*server, next_peer++, p_name));
		peers.back()->hello();
		return *peers.back();
	}

	void note_phase() {
		const net::MatchPhase p = phase_of(*server);
		if (phases.empty() || phases.back() != p) {
			phases.push_back(p);
		}
	}

	void tick(host::Rotation &r_rotation, const std::function<void(tt::ScriptedPeer &)> &p_answer) {
		out.clear();
		server->tick(out);
		note_syncs();
		for (auto &p : peers) {
			p->receive(out);
		}
		note_phase();
		if (std::unique_ptr<host::GameServer> next = r_rotation.after_tick(*server)) {
			out.clear();
			server = std::move(next);
			levels.push_back(r_rotation.current().level);
			for (auto &p : peers) {
				p->rebind(*server);
			}
			phases.push_back(net::MatchPhase::Loading);
		}
		for (auto &p : peers) {
			p_answer(*p);
		}
	}
};

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

	TEST_CASE(
			"rotation: three entries of Metro's data walk PreRound, InRound, RoundOver (RoundOverInfo names the next "
			"entry), LevelChange, the next server's load, PreRound; two peers keep slot, team and squad; the FullSync "
			"comes again after each Ready; a Hello during LevelChange gets the next entry's MatchInfo; a 1-entry "
			"MapList restarts in place") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const std::vector<std::string> names{ level->shared->level, level->shared->level + "-two",
			level->shared->level + "-three" };
		std::map<std::string, std::shared_ptr<const host::SharedLevel>> levels;
		host::MapList maps;
		for (const std::string &n : names) {
			levels[n] = n == names[0] ? level->shared : renamed(level->shared, n);
			maps.entries.push_back(host::LevelEntry{ n, level->shared->mode, 1 });
		}
		auto first = std::make_shared<const host::GameServerConfig>(metro_config(levels[names[0]], *level, true));
		host::RotationConfig rc;
		rc.maps = maps;
		rc.first = first;
		std::atomic<bool> gate{ false };
		std::atomic<uint32_t> loads{ 0 };
		rc.load = [&](const host::LevelEntry &p_entry) -> core::Result<std::shared_ptr<const host::GameServerConfig>> {
			if (loads.load() == 0) {
				gate.wait(false);
			}
			++loads;
			return std::make_shared<const host::GameServerConfig>(metro_config(levels.at(p_entry.level), *level, true));
		};
		host::Rotation rotation(rc);
		Rotated r;
		r.server = std::make_unique<host::GameServer>();
		REQUIRE(r.server->init(*first).ok());
		tt::ScriptedPeer &alpha = r.join("alpha");
		tt::ScriptedPeer &bravo = r.join("bravo");
		alpha.ready();
		bravo.ready();
		tt::ScriptedPeer *held = nullptr;
		std::map<uint32_t, uint32_t> answered;
		const auto answer = [&](tt::ScriptedPeer &p) {
			if (!p.match_info || p.match_info->phase == net::MatchPhase::LevelChange || p.match_info->round_id == 0) {
				return;
			}
			uint32_t &done = answered[p.peer()];
			if (done == p.match_info->round_id) {
				return;
			}
			done = p.match_info->round_id;
			p.loading(loading(done, 400));
			p.ready();
			r.readied(p, done);
		};
		struct Seen {
			int slot = -1;
			uint8_t team = 0;
			uint8_t squad = 0;
		};
		std::vector<std::map<uint32_t, Seen>> per_level;
		const auto snapshot = [&] {
			std::map<uint32_t, Seen> s;
			for (auto &p : r.peers) {
				Seen &e = s[p->peer()];
				e.slot = p->slot();
				e.team = e.slot >= 0 ? r.server->rules()->team_of(uint8_t(e.slot)) : uint8_t(0);
				e.squad = e.slot >= 0 ? r.server->rules()->squads().squad_of(uint8_t(e.slot)) : uint8_t(0);
			}
			per_level.push_back(std::move(s));
		};
		for (uint32_t played = 0; played < 3; ++played) {
			CAPTURE(played);
			bool in_round = false;
			for (uint32_t i = 0; i < kRoundTicks && !in_round; ++i) {
				r.tick(rotation, answer);
				in_round = phase_of(*r.server) == net::MatchPhase::InRound;
			}
			REQUIRE(in_round);
			snapshot();
			bool changed = false;
			const uint64_t changes = rotation.stats().level_changes;
			for (uint32_t i = 0; i < kRoundTicks + kIntermissionTicks && !changed; ++i) {
				r.tick(rotation, answer);
				if (held == nullptr && r.server->level_changing()) {
					held = &r.join("held");
					gate.store(true);
					gate.notify_all();
				}
				changed = rotation.stats().level_changes > changes;
			}
			REQUIRE(changed);
		}
		for (uint32_t i = 0; i < kSecond && r.synced_after_ready[alpha.peer()].size() < 3u; ++i) {
			r.tick(rotation, answer);
		}
		const host::RotationStats rs = rotation.stats();
		std::printf("rotation: %llu level changes, %llu in place, %u loads, round id %u, levels",
				(unsigned long long)rs.level_changes, (unsigned long long)rs.in_place, unsigned(loads.load()),
				rs.round_id);
		for (const std::string &l : r.levels) {
			std::printf(" %s", l.c_str());
		}
		std::printf("\nrotation: phases");
		for (const net::MatchPhase p : r.phases) {
			std::printf(" %s", host::match_phase_name(p));
		}
		std::printf("\n");
		CHECK(rs.level_changes == 3u);
		CHECK(rs.in_place == 0u);
		CHECK(rs.load_failures == 0u);
		CHECK(loads.load() == 3u);
		CHECK(r.levels == std::vector<std::string>{ names[1], names[2], names[0] });
		const std::vector<net::MatchPhase> cycle{ net::MatchPhase::PreRound, net::MatchPhase::InRound,
			net::MatchPhase::RoundOver, net::MatchPhase::LevelChange, net::MatchPhase::Loading };
		std::vector<net::MatchPhase> want;
		for (int i = 0; i < 3; ++i) {
			want.insert(want.end(), cycle.begin(), cycle.end());
		}
		std::vector<net::MatchPhase> seen;
		for (const net::MatchPhase p : r.phases) {
			if (p != net::MatchPhase::Restarting) {
				seen.push_back(p);
			}
		}
		seen.resize(std::min(seen.size(), want.size()));
		CHECK(seen == want);
		REQUIRE(held != nullptr);
		REQUIRE(held->match_infos.size() >= 2u);
		CHECK(held->match_infos.front().phase == net::MatchPhase::LevelChange);
		CHECK(held->match_infos.front().level == names[1]);
		CHECK(held->match_infos[1].level == names[1]);
		CHECK(held->match_infos[1].phase != net::MatchPhase::LevelChange);
		for (tt::ScriptedPeer *p : { &alpha, &bravo }) {
			CAPTURE(p->peer());
			CHECK_FALSE(p->kick.has_value());
			REQUIRE(p->round_overs.size() == 3u);
			CHECK(p->round_overs[0].next.level == names[1]);
			CHECK(p->round_overs[1].next.level == names[2]);
			CHECK(p->round_overs[2].next.level == names[0]);
			CHECK(p->round_overs[0].intermission_s == 60u);
			REQUIRE(p->level_changes.size() == 3u);
			CHECK(p->level_changes[0].level == names[1]);
			CHECK(p->level_changes[0].round_id == 1u);
			CHECK(p->level_changes[2].round_id == 3u);
			const std::vector<std::pair<uint32_t, bool>> want_syncs{ { 1u, true }, { 2u, true }, { 3u, true } };
			CHECK(r.synced_after_ready[p->peer()] == want_syncs);
		}
		REQUIRE(per_level.size() == 3u);
		for (size_t l = 1; l < per_level.size(); ++l) {
			CAPTURE(l);
			for (tt::ScriptedPeer *p : { &alpha, &bravo }) {
				const Seen &was = per_level[0].at(p->peer());
				const Seen &now = per_level[l].at(p->peer());
				CHECK(now.slot == was.slot);
				CHECK(now.team == was.team);
				CHECK(now.squad != 0u);
			}
			const bool mates0 = per_level[0].at(alpha.peer()).team == per_level[0].at(bravo.peer()).team &&
					per_level[0].at(alpha.peer()).squad == per_level[0].at(bravo.peer()).squad;
			const bool mates = per_level[l].at(alpha.peer()).team == per_level[l].at(bravo.peer()).team &&
					per_level[l].at(alpha.peer()).squad == per_level[l].at(bravo.peer()).squad;
			CHECK(mates == mates0);
		}

		host::RotationConfig single;
		single.maps.entries = { maps.entries[0] };
		single.first = first;
		single.background = false;
		single.load =
				[&](const host::LevelEntry &p_entry) -> core::Result<std::shared_ptr<const host::GameServerConfig>> {
			return std::make_shared<const host::GameServerConfig>(metro_config(levels.at(p_entry.level), *level, true));
		};
		host::Rotation one(single);
		host::GameServer server;
		REQUIRE(server.init(*first).ok());
		std::vector<host::OutDatagram> out;
		bool restarted = false;
		for (uint32_t i = 0; i < kRoundTicks + kIntermissionTicks && !restarted; ++i) {
			out.clear();
			server.tick(out);
			CHECK(one.after_tick(server) == nullptr);
			restarted = one.stats().in_place > 0;
		}
		CHECK(restarted);
		CHECK(one.stats().level_changes == 0u);
		CHECK_FALSE(server.level_ended());
		CHECK(server.rules()->stats().restarts == 1u);
	}

	TEST_CASE("install unchanged") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		tt::check_install_unchanged(*level);
	}
}
