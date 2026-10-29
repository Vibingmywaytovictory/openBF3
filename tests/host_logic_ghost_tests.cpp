#include <doctest/doctest.h>

#include "bf3/host/client_session.h"
#include "bf3/host/game_server.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/logic/logic_runtime.h"

#include "client_logic_sync.h"
#include "tdm_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace bf3;
namespace tt = bf3::test::tdm;
namespace lg = bf3::sim::logic;

namespace {

constexpr uint32_t kFirstJoinTick = 3600;
constexpr uint32_t kRoundOverJoinDelay = 120;

struct GhostState {
	size_t bool_count = 0;
	std::vector<uint64_t> bool_bits;
	std::vector<std::array<float, 12>> transforms;
	std::vector<sim::Tick> sequence_start;
	bool operator==(const GhostState &) const = default;
};

GhostState state_of(const lg::LogicGhost &p_ghost) {
	return GhostState{ p_ghost.bool_count, p_ghost.bool_bits, p_ghost.transforms, p_ghost.sequence_start };
}

struct Received {
	sim::Tick message_tick = 0;
	lg::LogicNetworkedEvent event;
};

bool same_event(const lg::LogicNetworkedEvent &a, const lg::LogicNetworkedEvent &b) {
	return a.tick == b.tick && a.connection == b.connection && a.source == b.source && a.source_port == b.source_port &&
			a.context.player == b.context.player && a.context.team == b.context.team;
}

struct LogicPeer {
	std::unique_ptr<tt::ScriptedPeer> peer;
	std::string name;
	sim::Tick hello_tick = 0;
	bool readied = false;
	std::optional<lg::LogicGhost> join_sync;
	uint8_t join_channel = 0xFF;
	std::optional<lg::LogicGhost> mirror;
	std::vector<Received> events;
	uint64_t deltas = 0, resyncs = 0, wrong_channel = 0, decode_errors = 0, early_deltas = 0, mirror_mismatches = 0;
	uint64_t mirror_checks = 0;
	host::ClientLogicSync client;

	void receive(const host::OutDatagram &p_d) {
		const std::optional<net::MessageType> type = net::peek_type(p_d.bytes);
		if (!type) {
			return;
		}
		if (*type == net::MessageType::FullSync || *type == net::MessageType::LogicDelta ||
				*type == net::MessageType::LogicEvents) {
			client.datagram(*type, p_d.bytes);
		}
		switch (*type) {
			case net::MessageType::FullSync: {
				core::Result<net::FullSync> f = net::decode_full_sync(p_d.bytes);
				if (!f) {
					++decode_errors;
					return;
				}
				if (!join_sync) {
					join_sync = f->ghost;
					join_channel = p_d.channel;
				} else {
					++resyncs;
					wrong_channel += p_d.channel != net::kChannelReliable;
				}
				mirror = f->ghost;
				return;
			}
			case net::MessageType::LogicDelta: {
				core::Result<net::LogicDelta> d = net::decode_logic_delta(p_d.bytes);
				if (!d) {
					++decode_errors;
					return;
				}
				++deltas;
				wrong_channel += p_d.channel != net::kChannelReliable;
				if (!mirror) {
					++early_deltas;
					return;
				}
				for (const lg::LogicGhost::BoolDelta &row : d->rows) {
					uint64_t &word = mirror->bool_bits[row.index / 64];
					const uint64_t bit = uint64_t(1) << (row.index % 64);
					word = row.value ? (word | bit) : (word & ~bit);
				}
				mirror->tick = d->tick;
				return;
			}
			case net::MessageType::LogicEvents: {
				core::Result<net::LogicEvents> e = net::decode_logic_events(p_d.bytes);
				if (!e) {
					++decode_errors;
					return;
				}
				wrong_channel += p_d.channel != net::kChannelReliable;
				for (const lg::LogicNetworkedEvent &ev : e->events) {
					events.push_back({ e->server_tick, ev });
				}
				return;
			}
			default:
				return;
		}
	}
};

}

TEST_SUITE("host-logic-ghost") {
	TEST_CASE("Metro TDM, 31 bots, a peer joining 60 s in and one during RoundOver: every networked event once, in "
			  "order, the ghosts equal the "
			  "server's") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		std::map<sim::Tick, std::vector<lg::LogicNetworkedEvent>> server_events;
		std::map<sim::Tick, GhostState> server_ghosts;
		std::set<uint64_t> connections;
		std::vector<LogicPeer> peers(2);
		peers[0].name = "joiner";
		peers[1].name = "round over joiner";
		sim::Tick round_over = 0;
		size_t ghost_changes = 0;
		std::optional<GhostState> last_ghost;
		size_t full_sync_bytes = 0;

		tt::TdmRunConfig c;
		c.seed = 3;
		c.bots = 31;
		c.peer = false;
		c.hashes = false;
		c.rules.max_players = 34;
		c.rules.kill_limit_override = 35;
		c.after_round_ticks = 3600 + 600;
		c.max_ticks = 36000;
		c.on_out = [&](const host::GameServer &p_server, std::span<const host::OutDatagram> p_out) {
			const host::TdmRules *rules = p_server.rules();
			REQUIRE(rules != nullptr);
			const sim::Tick now = p_server.current_tick();
			for (const lg::LogicNetworkedEvent &e : rules->networked()) {
				server_events[now].push_back(e);
				connections.insert(e.connection);
			}
			const std::optional<lg::LogicGhost> g = rules->ghost();
			REQUIRE(g.has_value());
			CHECK(g->tick == now);
			const GhostState s = state_of(*g);
			if (last_ghost && !(s == *last_ghost)) {
				++ghost_changes;
			}
			last_ghost = s;
			server_ghosts[now] = s;
			for (LogicPeer &p : peers) {
				if (p.peer == nullptr) {
					continue;
				}
				p.peer->receive(p_out);
				for (const host::OutDatagram &d : p_out) {
					if (d.peer == p.peer->peer()) {
						if (net::peek_type(d.bytes) == net::MessageType::FullSync && !p.join_sync) {
							full_sync_bytes = d.bytes.size();
						}
						p.receive(d);
					}
				}
				if (p.mirror) {
					++p.mirror_checks;
					if (!(state_of(*p.mirror) == s)) {
						++p.mirror_mismatches;
					}
				}
			}
		};
		c.on_tick = [&](sim::Tick p_now, host::GameServer &r_server, tt::ScriptedPeer *) {
			const host::TdmRules *rules = r_server.rules();
			if (round_over == 0 && rules->phase() == net::MatchPhase::RoundOver) {
				round_over = p_now;
			}
			const auto join = [&](LogicPeer &r_peer, uint32_t p_id) {
				if (r_peer.peer == nullptr) {
					r_peer.peer = std::make_unique<tt::ScriptedPeer>(r_server, p_id, r_peer.name);
					r_peer.peer->hello();
					r_peer.hello_tick = p_now;
				} else if (!r_peer.readied && r_peer.peer->match_info) {
					r_peer.peer->ready();
					r_peer.readied = true;
				}
			};
			if (p_now >= kFirstJoinTick) {
				join(peers[0], 1u);
			}
			if (round_over != 0 && p_now >= round_over + kRoundOverJoinDelay) {
				join(peers[1], 2u);
			}
		};
		core::Result<tt::TdmRunResult> run = tt::run_tdm(*level, c);
		REQUIRE_MESSAGE(run.ok(), (run.ok() ? std::string() : run.error().message));
		REQUIRE_MESSAGE(run->failure.empty(), run->failure);
		REQUIRE(run->round_over);
		size_t total = 0;
		for (const auto &[tick, list] : server_events) {
			total += list.size();
		}
		std::printf("host.logic.ghost: %u ticks, round over at %u, %zu networked events at %zu ticks over %zu "
					"connections, %zu ghost "
					"changes, ghost %zu bools / %zu transforms / %zu sequences, join FullSync %zu B, networked_dropped "
					"%llu\n",
				run->ticks, round_over, total, server_events.size(), connections.size(), ghost_changes,
				last_ghost ? last_ghost->bool_count : size_t(0), last_ghost ? last_ghost->transforms.size() : size_t(0),
				last_ghost ? last_ghost->sequence_start.size() : size_t(0), full_sync_bytes,
				(unsigned long long)run->rules.networked_dropped);
		CHECK(run->rules.networked_dropped == 0);
		CHECK(round_over > kFirstJoinTick + 600);
		CHECK(total > 0);
		for (LogicPeer &p : peers) {
			INFO(p.name);
			REQUIRE(p.peer != nullptr);
			REQUIRE(p.readied);
			REQUIRE(p.join_sync.has_value());
			CHECK(p.join_channel == net::kChannelBulk);
			CHECK(p.decode_errors == 0);
			CHECK(p.wrong_channel == 0);
			CHECK(p.early_deltas == 0);
			CHECK(p.peer->decode_errors == 0);
			const sim::Tick sync_tick = p.join_sync->tick;
			REQUIRE(server_ghosts.count(sync_tick) == 1);
			CHECK(state_of(*p.join_sync) == server_ghosts[sync_tick]);
			CHECK(p.mirror_checks > 0);
			CHECK(p.mirror_mismatches == 0);
			std::vector<Received> expected;
			for (const auto &[tick, list] : server_events) {
				if (tick > sync_tick) {
					for (const lg::LogicNetworkedEvent &e : list) {
						expected.push_back({ tick, e });
					}
				}
			}
			size_t older = 0;
			for (const Received &r : p.events) {
				older += r.event.tick <= sync_tick;
			}
			CHECK(older == 0);
			REQUIRE(p.events.size() == expected.size());
			size_t wrong = 0;
			for (size_t i = 0; i < expected.size(); ++i) {
				wrong += !(p.events[i].message_tick == expected[i].message_tick &&
						same_event(p.events[i].event, expected[i].event) &&
						p.events[i].event.tick == p.events[i].message_tick);
			}
			CHECK(wrong == 0);
			host::ClientLogicFeed feed;
			p.client.take(feed);
			host::ClientStats cs;
			p.client.stats(cs);
			CHECK(cs.logic_full_syncs == 1 + p.resyncs);
			CHECK(cs.logic_deltas == p.deltas);
			CHECK(cs.logic_events == expected.size());
			CHECK(cs.logic_early_events == 0);
			CHECK(cs.logic_stale == 0);
			CHECK(cs.logic_dropped_events == 0);
			CHECK(cs.bad_logic_datagrams == 0);
			REQUIRE(feed.ghost.has_value());
			CHECK(state_of(*feed.ghost) == *last_ghost);
			REQUIRE(feed.events.size() == expected.size());
			size_t fed_wrong = 0;
			for (size_t i = 0; i < expected.size(); ++i) {
				fed_wrong += !same_event(feed.events[i], expected[i].event);
			}
			CHECK(fed_wrong == 0);
			host::ClientLogicFeed again = feed;
			again.events.clear();
			again.ghost.reset();
			p.client.take(again);
			CHECK(again.events.empty());
			CHECK_FALSE(again.ghost.has_value());
			std::printf("host.logic.ghost: %s: hello at %u, FullSync of tick %u on channel %u, %zu events, %llu "
						"deltas, %llu resyncs\n",
					p.name.c_str(), p.hello_tick, sync_tick, unsigned(p.join_channel), p.events.size(),
					(unsigned long long)p.deltas, (unsigned long long)p.resyncs);
		}
		CHECK(peers[0].events.size() > peers[1].events.size());
		CHECK(peers[1].events.size() > 0);
	}

	TEST_CASE("host-logic-ghost: install unchanged") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level != nullptr) {
			tt::check_install_unchanged(*level);
		}
	}
}
