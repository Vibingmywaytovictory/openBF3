#include <doctest/doctest.h>

#include "bf3/host/bot_fill.h"
#include "bf3/host/game_server.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include "tdm_fixture.h"
#include "test_support.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

using namespace bf3;
namespace tt = bf3::test::tdm;

namespace {

constexpr uint64_t kTdmStreamHash = 0xdc05b58de9e6e28full;
constexpr uint64_t kTdmStateHash = 0x801d84e3f1ae9e0full;
constexpr uint64_t kDevStreamHash = 0x09e4ad77570eefd6ull;
constexpr uint64_t kDevStateHash = 0xbb55f06d887305ceull;

struct StreamRecorder {
	std::vector<uint8_t> hold_mask = tt::detail::echo_hold_mask();
	tt::detail::Fnv stream;
	tt::detail::Fnv states;
	uint64_t datagrams = 0;
	uint64_t bytes = 0;
	std::array<uint64_t, 256> types{};
	std::vector<uint8_t> scratch;

	void record(sim::Tick p_tick, std::span<const host::OutDatagram> p_out, const host::GameServer &p_server, uint32_t p_slots) {
		for (const host::OutDatagram &d : p_out) {
			const std::optional<net::MessageType> type = net::peek_type(d.bytes);
			const uint8_t tag = type ? uint8_t(*type) : uint8_t(0);
			++types[tag];
			++datagrams;
			bytes += d.bytes.size();
			scratch = d.bytes;
			if (type == net::MessageType::Snapshot) {
				for (size_t i = 0; i < hold_mask.size() && i < scratch.size(); ++i) {
					scratch[i] = uint8_t(scratch[i] & ~hold_mask[i]);
				}
			}
			stream.add(p_tick);
			stream.add(d.peer);
			stream.add(d.channel);
			stream.add(d.reliable);
			stream.add(uint64_t(scratch.size()));
			stream.bytes(scratch.data(), scratch.size());
		}
		const host::Gunplay *gunplay = p_server.gunplay();
		states.add(p_tick);
		for (uint32_t s = 0; s < p_slots; ++s) {
			const sim::SoldierState *state = p_server.soldier(s);
			if (state == nullptr) {
				continue;
			}
			states.add(s);
			states.add(sim::soldier_state_hash(*state));
			if (gunplay != nullptr) {
				states.add(sim::weapons::soldier_weapons_hash(gunplay->weapons(uint8_t(s))));
			}
			states.add(p_server.view().team(uint8_t(s)));
			states.add(p_server.view().alive(uint8_t(s)));
			states.add(p_server.view().kit(uint8_t(s)));
		}
	}

	void finish(const host::GameServer &p_server) {
		const host::GameServerStats g = p_server.stats();
		states.add(g.tick);
		states.add(g.clients);
		states.add(g.soldiers);
		states.add(g.snapshots_sent);
		states.add(g.snapshot_bytes);
		states.add(uint64_t(g.max_snapshot_bytes));
		states.add(g.bad_datagrams);
		states.add(g.spawn_failures);
		states.add(g.level_mismatch_kicks);
		states.add(g.rules_step_failures);
		states.add(g.step_precondition_failures);
		states.add(g.motor_steps);
		for (const host::SlotStats &s : g.slots) {
			states.add(s.used);
			states.add(s.remote);
			states.add(s.peer);
			states.add(s.life);
			states.add(s.alive);
			states.add(s.commands_applied);
			states.add(s.commands_repeated);
			states.add(s.commands_late);
			states.add(s.commands_replaced);
			states.add(s.buffer_ticks);
		}
		states.add(g.gunplay_active);
		states.add(g.gunplay.shots);
		states.add(g.gunplay.hits);
		states.add(g.gunplay.deaths);
		states.add(g.gunplay.kit_select);
		states.add(g.gunplay.kit_select_refused);
		states.add(g.gunplay.team_change_refused);
		states.add(g.gunplay.shot_events_dropped);
		states.add(g.gunplay.combat_events_clamped);
		states.add(g.gunplay.others_dropped);
		states.add(g.gunplay.trace_dropped);
		states.add(g.match_seed);
		states.add(g.weapon_hash);
		states.add(g.kit_hash);
		states.add(g.weapon_flags);
	}

	uint64_t count(net::MessageType p_type) const { return types[uint8_t(p_type)]; }

	void print(const char *p_name) const {
		std::printf("%s: %llu datagrams, %llu bytes; snapshots %llu, others %llu, shots %llu, combat %llu, spawned %llu, debug reports %llu, kicks %llu, "
					"match state %llu, player lists %llu; stream %016llx, states %016llx\n",
				p_name, (unsigned long long)datagrams, (unsigned long long)bytes, (unsigned long long)count(net::MessageType::Snapshot),
				(unsigned long long)count(net::MessageType::SnapshotOthers), (unsigned long long)count(net::MessageType::ShotEvents),
				(unsigned long long)count(net::MessageType::CombatEvents), (unsigned long long)count(net::MessageType::Spawned),
				(unsigned long long)count(net::MessageType::DebugReport), (unsigned long long)count(net::MessageType::Kick),
				(unsigned long long)count(net::MessageType::MatchState), (unsigned long long)count(net::MessageType::PlayerList),
				(unsigned long long)stream.h, (unsigned long long)states.h);
	}
};

sim::Command scripted_command(sim::Tick p_tick, uint32_t p_phase) {
	sim::Command c;
	c.move_forward = sim::kAxisMax;
	c.move_right = int8_t(((p_tick / 90u + p_phase) % 3u) == 0 ? 64 : 0);
	c.yaw = uint16_t((p_tick + p_phase * 977u) * 211u);
	c.pitch = int16_t(int32_t((p_tick + p_phase * 31u) % 400u) - 200);
	c.buttons = ((p_tick + p_phase * 13u) % 50u) < 12u ? sim::buttons::kFire : 0u;
	c.pose = ((p_tick / 240u + p_phase) % 4u) == 3u ? sim::Pose::Crouch : sim::Pose::Stand;
	return c;
}

void send_packet(host::GameServer &r_server, const tt::ScriptedPeer &p_peer, std::span<const sim::Command> p_commands) {
	net::CommandPacket p;
	p.count = uint8_t(p_commands.size());
	for (size_t i = 0; i < p_commands.size(); ++i) {
		p.commands[i] = p_commands[i];
	}
	if (p_peer.last_snapshot) {
		p.newest_snapshot = p_peer.last_snapshot->seq;
		p.snapshot_ack_bits = 0xFFFFFFFFu;
		p.others_ack_bits = 0x0000FFFFu;
	}
	r_server.on_receive(p_peer.peer(), net::kChannelUnreliable, net::encode_command_packet(p));
}

net::DebugRequest debug_request(net::DebugKind p_kind, uint32_t p_id) {
	net::DebugRequest q;
	q.kind = p_kind;
	q.id = p_id;
	return q;
}

}

TEST_SUITE("host-server-stream") {
	TEST_CASE("TDM: 30 bots and two scripted peers, 1200 ticks: every datagram and every tick's soldiers hash to the recorded stream") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		host::GameServerConfig cfg;
		cfg.level = level->shared;
		cfg.max_slots = 40;
		cfg.dev_commands = true;
		cfg.seed = 21;
		cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		cfg.rules.mode = host::MatchRulesConfig::Mode::Tdm;
		cfg.rules.deploy_screen = true;
		cfg.rules.revive = true;
		cfg.rules.preround_override_s = 0.0f;
		cfg.rules.max_players = 32;
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = level->bots;
		fill->count = 0;
		cfg.bots = fill;
		auto bots = std::make_shared<host::TdmBotConfig>();
		bots->count = 30;
		bots->tuning_cfg = OPENBF3_BOT_TUNING_CFG;
		cfg.tdm_bots = bots;
		host::GameServer server;
		const core::Status st = server.init(cfg);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));

		StreamRecorder rec;
		std::vector<host::OutDatagram> out;
		tt::ScriptedPeer alpha(server, 1u, "alpha");
		tt::ScriptedPeer bravo(server, 2u, "bravo");
		const auto step = [&] {
			out.clear();
			server.tick(out);
			alpha.receive(out);
			bravo.receive(out);
			rec.record(server.current_tick(), out, server, cfg.max_slots);
		};
		alpha.hello();
		bravo.hello();
		step();
		alpha.ready();
		bravo.ready();
		alpha.spawn_request(1);
		bravo.spawn_request(2);
		uint32_t alpha_seq = 0, bravo_seq = 0;
		bool bravo_connected = true;
		for (sim::Tick i = 0; i < 1200; ++i) {
			const sim::Tick next = server.current_tick() + 1u;
			for (tt::ScriptedPeer *p : { &alpha, &bravo }) {
				if (p == &bravo && !bravo_connected) {
					continue;
				}
				const int slot = p->slot();
				const sim::SoldierState *own = slot >= 0 ? server.soldier(uint32_t(slot)) : nullptr;
				const bool alive = own != nullptr && (own->flags & sim::soldier_flags::kAlive) != 0;
				if (!alive && next % 30u == 0u) {
					p->spawn_request(p == &alpha ? uint8_t(1) : uint8_t(next / 30u % 4u));
				}
			}
			sim::Command a = scripted_command(next, 0);
			a.tick = next;
			a.sequence = ++alpha_seq;
			if (next % 7u == 0u) {
				sim::Command ahead = scripted_command(next + 1u, 0);
				ahead.tick = next + 1u;
				ahead.sequence = ++alpha_seq;
				const sim::Command pair[2] = { a, ahead };
				send_packet(server, alpha, pair);
			} else {
				send_packet(server, alpha, std::span<const sim::Command>(&a, 1));
			}
			if (bravo_connected && next % 5u != 3u) {
				sim::Command b = scripted_command(next, 1);
				b.tick = next + 2u;
				b.sequence = ++bravo_seq;
				send_packet(server, bravo, std::span<const sim::Command>(&b, 1));
				if (next % 11u == 0u) {
					sim::Command again = b;
					again.sequence = ++bravo_seq;
					again.buttons ^= sim::buttons::kFire;
					send_packet(server, bravo, std::span<const sim::Command>(&again, 1));
				}
				if (next % 13u == 0u) {
					sim::Command late = b;
					late.tick = next - 1u;
					late.sequence = ++bravo_seq;
					send_packet(server, bravo, std::span<const sim::Command>(&late, 1));
				}
			}
			if (next == 300u) {
				net::DebugRequest q = debug_request(net::DebugKind::Trace, 1);
				q.ticks = 40;
				alpha.debug(q);
			}
			if (next == 400u || next == 1100u) {
				net::DebugRequest q = debug_request(net::DebugKind::TargetInfo, next);
				q.slot = 0;
				alpha.debug(q);
			}
			if (next == 500u) {
				net::KitSelect k;
				k.kit = 2;
				bravo.kit_select(k);
			}
			if (next == 600u) {
				const uint8_t junk[3] = { 0xFF, 0x01, 0x02 };
				server.on_receive(alpha.peer(), net::kChannelReliable, junk);
			}
			if (next == 900u) {
				server.on_receive(bravo.peer(), net::kChannelReliable, net::encode_bye());
				bravo_connected = false;
			}
			if (next == 960u) {
				bravo.hello();
				bravo.ready();
				bravo.spawn_request(3);
				bravo_connected = true;
			}
			step();
		}
		rec.finish(server);
		rec.print("host.server.stream TDM");
		CHECK(alpha.decode_errors == 0u);
		CHECK(bravo.decode_errors == 0u);
		CHECK(rec.count(net::MessageType::Snapshot) > 0u);
		CHECK(rec.count(net::MessageType::ShotEvents) > 0u);
		CHECK(rec.count(net::MessageType::CombatEvents) > 0u);
		CHECK(rec.count(net::MessageType::Spawned) > 0u);
		CHECK(rec.count(net::MessageType::DebugReport) >= 3u);
		CHECK(rec.count(net::MessageType::MatchState) > 0u);
		CHECK(rec.count(net::MessageType::PlayerList) > 0u);
		CHECK(server.stats().bad_datagrams >= 1u);
		CHECK(rec.stream.h == kTdmStreamHash);
		CHECK(rec.states.h == kTdmStateHash);
	}

	TEST_CASE("dev server: walkers, target bots, fill bots, a peer's dev requests and a spawn sweep, 900 ticks: the recorded stream") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		host::GameServerConfig cfg;
		cfg.level = level->shared;
		cfg.max_slots = 56;
		cfg.dev_commands = true;
		cfg.seed = 5;
		cfg.walkers = 44;
		cfg.respawn_ticks = 45;
		cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		cfg.gunplay.target_bots = 2;
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = level->bots;
		fill->count = 2;
		cfg.bots = fill;
		host::GameServer server;
		const core::Status st = server.init(cfg);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));

		StreamRecorder rec;
		std::vector<host::OutDatagram> out;
		tt::ScriptedPeer alpha(server, 1u, "alpha");
		std::vector<std::unique_ptr<tt::ScriptedPeer>> crowd;
		const auto step = [&] {
			out.clear();
			server.tick(out);
			alpha.receive(out);
			for (const std::unique_ptr<tt::ScriptedPeer> &p : crowd) {
				p->receive(out);
			}
			rec.record(server.current_tick(), out, server, cfg.max_slots);
		};
		alpha.hello();
		step();
		alpha.ready();
		const std::vector<sim::SpawnPoint> &spawns = level->shared->spawns;
		REQUIRE(spawns.size() > 8u);
		uint32_t seq = 0;
		for (sim::Tick i = 0; i < 900; ++i) {
			const sim::Tick next = server.current_tick() + 1u;
			sim::Command a = scripted_command(next, 2);
			a.tick = next;
			a.sequence = ++seq;
			send_packet(server, alpha, std::span<const sim::Command>(&a, 1));
			if (next == 2u) {
				net::Hello h;
				h.protocol = uint16_t(net::kProtocolVersion + 1u);
				h.name = "old";
				server.on_receive(30u, net::kChannelReliable, net::encode_hello(h));
			}
			if (next == 3u) {
				net::Hello h;
				h.name = "other level";
				server.on_receive(31u, net::kChannelReliable, net::encode_hello(h));
				net::Ready r;
				core::Sha256Digest digest{};
				digest.bytes.fill(0x5A);
				r.source_digest = digest;
				server.on_receive(31u, net::kChannelReliable, net::encode_ready(r));
			}
			if (next == 50u) {
				net::DebugRequest q = debug_request(net::DebugKind::SetSeed, 50);
				q.index = 7;
				alpha.debug(q);
			}
			if (next == 60u) {
				net::DebugRequest q = debug_request(net::DebugKind::SpawnAt, 60);
				q.index = 3;
				alpha.debug(q);
				q.id = 61;
				q.index = 100000;
				alpha.debug(q);
			}
			if (next == 120u) {
				net::DebugRequest q = debug_request(net::DebugKind::Teleport, 120);
				q.position = spawns[5].position;
				q.yaw = 12000;
				alpha.debug(q);
			}
			if (next == 150u) {
				net::DebugRequest q = debug_request(net::DebugKind::SpawnTarget, 150);
				q.slot = 0;
				q.position = spawns[8].position;
				q.yaw = 30000;
				q.pose = sim::Pose::Crouch;
				alpha.debug(q);
			}
			if (next == 160u) {
				net::DebugRequest q = debug_request(net::DebugKind::TargetInfo, 160);
				q.slot = 1;
				alpha.debug(q);
			}
			if (next == 200u) {
				net::DebugRequest q = debug_request(net::DebugKind::Trace, 200);
				q.ticks = 30;
				alpha.debug(q);
			}
			if (next == 260u) {
				net::KitSelect k;
				k.kit = 3;
				k.team = 2;
				alpha.kit_select(k);
			}
			if (next == 300u) {
				net::DebugRequest q = debug_request(net::DebugKind::SpawnSweep, 300);
				q.ticks = 3;
				alpha.debug(q);
			}
			if (next == 400u) {
				const core::Status placed = server.place_soldier(2, spawns[2].position, 4000, sim::Pose::Prone);
				CHECK_MESSAGE(placed.ok(), (placed.ok() ? std::string() : placed.error().message));
			}
			if (next == 600u) {
				for (uint32_t p = 0; p < 8; ++p) {
					crowd.push_back(std::make_unique<tt::ScriptedPeer>(server, 40u + p, "crowd"));
					crowd.back()->hello();
					crowd.back()->ready();
				}
				net::KitSelect k;
				k.kit = 1;
				server.on_receive(99u, net::kChannelReliable, net::encode_kit_select(k));
				net::SpawnRequest q;
				q.kit = 1;
				server.on_receive(alpha.peer(), net::kChannelReliable, net::encode_spawn_request(q));
			}
			if (next == 700u) {
				for (const std::unique_ptr<tt::ScriptedPeer> &p : crowd) {
					server.on_disconnect(p->peer());
				}
			}
			step();
		}
		rec.finish(server);
		rec.print("host.server.stream dev");
		CHECK(alpha.decode_errors == 0u);
		CHECK(rec.count(net::MessageType::Kick) >= 3u);
		CHECK(rec.count(net::MessageType::SnapshotOthers) > 0u);
		CHECK(rec.count(net::MessageType::DebugReport) >= 7u);
		CHECK(rec.count(net::MessageType::Spawned) > 0u);
		CHECK(server.stats().level_mismatch_kicks == 1u);
		CHECK(server.stats().bad_datagrams >= 2u);
		CHECK(rec.stream.h == kDevStreamHash);
		CHECK(rec.states.h == kDevStateHash);
	}

	TEST_CASE("host-server-stream: install unchanged") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level != nullptr) {
			tt::check_install_unchanged(*level);
		}
	}
}
