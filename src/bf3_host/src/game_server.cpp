#include "bf3/host/game_server.h"

#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/splitmix64.h"
#include "bf3/host/firefight_brain.h"
#include "bf3/host/tdm_brain.h"
#include "bot_director.h"

#include "bf3/net/loopback_ping.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>

namespace bf3::host {

namespace {

using bf3::core::ms_since;
using bf3::core::splitmix64_next;

float distance2(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return dx * dx + dy * dy + dz * dz;
}

bool alive(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead;
}

class Walker final : public CommandSource {
public:
	explicit Walker(uint32_t p_phase) :
			phase_(p_phase) {}
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override {
		if (!started_) {
			started_ = true;
			base_yaw_ = p_state.yaw;
			start_ = p_tick;
		}
		sim::Command c;
		c.tick = p_tick;
		c.move_forward = sim::kAxisMax;
		const uint32_t leg = ((p_tick - start_) / (2u * sim::kSimHz) + phase_) & 3u;
		c.yaw = uint16_t(base_yaw_ + leg * 16384u);
		c.pose = sim::Pose::Stand;
		return c;
	}

private:
	uint32_t phase_ = 0;
	bool started_ = false;
	uint16_t base_yaw_ = 0;
	sim::Tick start_ = 0;
};

constexpr uint32_t kMaxSweepTicks = 600;

constexpr size_t kMaxQueuedDebug = 64;
constexpr size_t kMaxPendingReplies = 1024;

}

struct GameServer::Impl {
	struct Buffered {
		bool valid = false;
		sim::Command command;
	};

	struct Slot {
		bool used = false;
		bool remote = false;
		bool dummy = false;
		bool bot = false;
		uint32_t peer = 0;
		int team = 1;
		bool hello = false;
		bool spawned = false;
		bool want_spawn = false;
		sim::Tick respawn_at = 0;
		uint32_t spawn_index = 0;
		sim::SoldierState state;
		sim::Command last;
		uint32_t applied_sequence = 0;
		bool applied_repeated = true;
		std::vector<Buffered> buffer;
		bool any_command = false;
		uint32_t highest_sequence = 0;
		sim::Tick highest_sequence_tick = 0;
		uint64_t highest_sequence_ns = 0;
		uint32_t last_applied_sequence = 0;
		uint32_t highest_late_sequence = 0;
		uint16_t snapshot_seq = 0;
		uint16_t own_events = 0;
		std::unique_ptr<CommandSource> source;
		sim::Command source_command;
		SlotStats stats;

		bool tracing = false;
		uint32_t trace_id = 0;
		uint32_t trace_ticks = 0;
		std::vector<sim::SoldierState> trace;

		size_t sweep_entry = 0;
	};

	struct PendingDebug {
		uint32_t slot = 0;
		net::DebugRequest request;
	};

	struct Sweep {
		bool active = false;
		uint32_t requester = 0;
		uint32_t id = 0;
		uint32_t ticks = 60;
		size_t next_point = 0;
		bool batch_active = false;
		sim::Tick batch_start = 0;
		std::vector<sim::SettleResult> results;
		std::vector<uint32_t> batch_points;
		std::vector<net::SpawnSweepRow> rows;
	};

	GameServerConfig config;
	std::unique_ptr<sim::CollisionWorld> world;
	sim::SoldierMotor motor;
	uint64_t motor_hash = 0;
	std::vector<Slot> slots;
	sim::Tick tick = 0;
	uint64_t rng = 1;
	std::vector<OutDatagram> pending_out;
	std::deque<PendingDebug> debug_queue;
	Sweep sweep;
	uint64_t snapshots_sent = 0;
	uint64_t snapshot_bytes = 0;
	size_t max_snapshot_bytes = 0;
	uint64_t bad_datagrams = 0;
	uint64_t spawn_failures = 0;
	uint64_t level_mismatch_kicks = 0;
	uint64_t rules_step_failures = 0;
	uint64_t step_precondition_failures = 0;
	double last_step_ms = 0;
	double last_encode_ms = 0;

	GameServerTickTimes times;
	std::unique_ptr<BotDirector> bots;
	uint64_t motor_steps = 0;
	uint32_t tick_spawns = 0;

	int slot_of_peer(uint32_t p_peer) const {
		for (size_t i = 0; i < slots.size(); ++i) {
			if (slots[i].used && slots[i].remote && slots[i].peer == p_peer) {
				return int(i);
			}
		}
		return -1;
	}

	int free_slot() const {
		for (size_t i = 0; i < slots.size(); ++i) {
			if (!slots[i].used) {
				return int(i);
			}
		}
		return -1;
	}

	void reset_slot(Slot &r_slot) {
		r_slot = Slot();
		r_slot.buffer.assign(std::clamp<uint32_t>(config.jitter_buffer_ticks, 1u, 1024u) + 1u, Buffered{});
	}

	void send(uint32_t p_peer, uint8_t p_channel, std::vector<uint8_t> p_bytes) {
		OutDatagram d;
		d.peer = p_peer;
		d.channel = p_channel;
		d.reliable = p_channel != net::kChannelUnreliable;
		d.bytes = std::move(p_bytes);
		pending_out.push_back(std::move(d));
	}

	void report(uint32_t p_slot, uint32_t p_id, bool p_ok, std::string p_text) {
		if (p_slot >= slots.size()) {
			return;
		}
		const Slot &s = slots[p_slot];
		if (!s.used || !s.remote || pending_out.size() >= kMaxPendingReplies) {
			return;
		}
		net::DebugReport r;
		r.id = p_id;
		r.ok = p_ok;
		r.text = std::move(p_text);
		send(s.peer, net::kChannelReliable, net::encode_debug_report(r));
	}

	std::vector<sim::Vec3> live_positions(size_t p_except) const {
		std::vector<sim::Vec3> out;
		for (size_t i = 0; i < slots.size(); ++i) {
			const Slot &s = slots[i];
			if (i != p_except && s.used && !s.dummy && s.spawned && alive(s.state)) {
				out.push_back(s.state.position);
			}
		}
		return out;
	}

	bool spawn(size_t p_slot, size_t p_point, sim::Tick p_tick) {
		Slot &s = slots[p_slot];

		const std::vector<sim::SpawnPoint> &points =
				s.bot && config.bots != nullptr && config.bots->spawn_in_main && config.bots->level != nullptr ? config.bots->level->spawns.points
																												 : config.level->spawns;
		if (points.empty()) {
			++spawn_failures;
			return false;
		}
		size_t index = p_point;
		if (index == SIZE_MAX) {
			index = sv_choose_spawn(p_slot, points, p_tick);
		}
		if (index == SIZE_MAX) {
			index = sim::choose_spawn(points, s.team, live_positions(p_slot), config.level->spawn_protection_radius,
					splitmix64_next(rng));
			if (index == SIZE_MAX) {
				index = size_t(splitmix64_next(rng) % points.size());
			}
		}
		if (index >= points.size()) {
			++spawn_failures;
			return false;
		}
		const sim::SettleResult r = sim::settle_spawn(motor, *world, points[index], s.state.life + 1u, p_tick);
		++tick_spawns;
		if (!r.ok) {
			std::fprintf(stderr, "GameServer: spawn point %u did not settle: %s (spawning there anyway)\n", points[index].index, r.failure.c_str());
		}
		s.state = r.state;
		s.spawned = true;
		s.want_spawn = false;
		s.spawn_index = uint32_t(index);
		s.last = sim::hold_command(s.state);
		s.own_events = uint16_t(s.own_events | s.state.events);
		sv_on_spawn(p_slot, p_tick);
		if (s.remote) {
			net::Spawned m;
			m.slot = uint8_t(p_slot);
			m.spawn_index = points[index].index;
			m.state = s.state;
			sv_spawned(p_slot, m);
			send(s.peer, net::kChannelReliable, net::encode_spawned(m));
		}
		return true;
	}

	void receive_commands(Slot &r_slot, const net::CommandPacket &p_packet, uint64_t p_now_ns) {
		sv_ack(r_slot, p_packet);
		const uint32_t size = uint32_t(r_slot.buffer.size());
		const uint32_t ahead = std::clamp<uint32_t>(config.jitter_buffer_ticks, 1u, 1024u);

		for (size_t k = p_packet.count; k-- > 0;) {
			sim::Command c = p_packet.commands[k];
			c.flags = 0;
			if (!r_slot.any_command || c.sequence > r_slot.highest_sequence) {
				r_slot.any_command = true;
				r_slot.highest_sequence = c.sequence;
				r_slot.highest_sequence_tick = c.tick;
				r_slot.highest_sequence_ns = p_now_ns;
			}
			if (c.tick <= tick) {

				if (c.sequence > r_slot.last_applied_sequence && c.sequence > r_slot.highest_late_sequence) {
					r_slot.highest_late_sequence = c.sequence;
					++r_slot.stats.commands_late;
				}
				continue;
			}
			if (c.tick - tick > ahead) {
				continue;
			}
			Buffered &b = r_slot.buffer[c.tick % size];
			if (!b.valid || b.command.tick != c.tick) {
				b.valid = true;
				b.command = c;
				sv_buffered(r_slot, c);
			} else if (c.sequence > b.command.sequence) {

				b.command = c;
				++r_slot.stats.commands_replaced;
				sv_buffered(r_slot, c);
			}
		}
	}

	void handle_hello(uint32_t p_peer, const net::Hello &p_hello) {
		if (slot_of_peer(p_peer) >= 0) {
			return;
		}
		if (p_hello.protocol != net::kProtocolVersion) {
			net::Kick k;
			k.reason = "protocol " + std::to_string(p_hello.protocol) + " (server " + std::to_string(net::kProtocolVersion) + ")";
			send(p_peer, net::kChannelReliable, net::encode_kick(k));
			return;
		}
		const int at = free_slot();
		if (at < 0) {
			net::Kick k;
			k.reason = "server full";
			send(p_peer, net::kChannelReliable, net::encode_kick(k));
			return;
		}
		Slot &s = slots[size_t(at)];
		reset_slot(s);
		s.used = true;
		s.remote = true;
		s.peer = p_peer;
		s.team = sv_rules_on() ? int(sv_connect(size_t(at), false, p_hello.name)) : 1;
		s.hello = true;
		net::MatchInfo m;
		m.level = config.level->level;
		m.mode = config.level->mode;
		m.snapshot_divisor = config.snapshot_divisor;
		m.slot = uint8_t(at);
		m.server_tick = tick;
		m.phase = net::MatchPhase::InRound;
		m.source_digest = config.level->source_digest;
		m.motor_hash = motor_hash;
		sv_match_info(size_t(at), m);
		sv_rules_match_info(m);
		send(p_peer, net::kChannelReliable, net::encode_match_info(m));
		sv_rules_join(size_t(at));
	}

	void handle_debug(size_t p_slot, const net::DebugRequest &p_request) {
		if (!config.dev_commands) {
			report(uint32_t(p_slot), p_request.id, false, "dev commands are disabled on this server");
			return;
		}
		Slot &s = slots[p_slot];
		switch (p_request.kind) {
			case net::DebugKind::Trace:
				if (p_request.ticks == 0 || p_request.ticks > net::kMaxTraceTicks) {
					report(uint32_t(p_slot), p_request.id, false, "trace: ticks must be 1.." + std::to_string(net::kMaxTraceTicks));
					return;
				}
				if (s.tracing) {
					report(uint32_t(p_slot), p_request.id, false, "trace: one is already running");
					return;
				}
				s.tracing = true;
				s.trace_id = p_request.id;
				s.trace_ticks = p_request.ticks;
				s.trace.clear();
				s.trace.reserve(p_request.ticks);
				return;
			case net::DebugKind::SetSeed:
				rng = uint64_t(p_request.index) * 0x9E3779B97F4A7C15ull + 1u;
				report(uint32_t(p_slot), p_request.id, true, "seed set");
				return;
			case net::DebugKind::SpawnTarget:
			case net::DebugKind::TargetInfo:
			case net::DebugKind::Teleport:
			case net::DebugKind::SpawnAt:
			case net::DebugKind::SpawnSweep:
				if (debug_queue.size() >= kMaxQueuedDebug) {
					report(uint32_t(p_slot), p_request.id, false, "too many queued requests");
					return;
				}
				debug_queue.push_back({ uint32_t(p_slot), p_request });
				return;
		}
		report(uint32_t(p_slot), p_request.id, false, "unknown debug request");
	}

	void apply_debug(sim::Tick p_tick) {
		while (!debug_queue.empty()) {
			const PendingDebug d = debug_queue.front();
			debug_queue.pop_front();
			Slot &s = slots[d.slot];
			if (!s.used) {
				continue;
			}
			const net::DebugRequest &q = d.request;
			switch (q.kind) {
				case net::DebugKind::Teleport: {
					if (!s.spawned) {
						report(d.slot, q.id, false, "teleport: no soldier");
						break;
					}

					s.state = motor.spawn_state(q.position, q.yaw, s.state.life, p_tick, false);
					char text[128];
					std::snprintf(text, sizeof(text), "teleported to (%.3f, %.3f, %.3f) at tick %u", double(q.position[0]), double(q.position[1]),
							double(q.position[2]), p_tick);
					report(d.slot, q.id, true, text);
					break;
				}
				case net::DebugKind::SpawnAt:
					if (q.index >= config.level->spawns.size()) {
						report(d.slot, q.id, false, "spawn at: no point " + std::to_string(q.index));
						break;
					}
					if (!spawn(d.slot, q.index, p_tick)) {
						report(d.slot, q.id, false, "spawn at: the slot could not spawn at point " + std::to_string(q.index));
						break;
					}
					report(d.slot, q.id, true, "spawned at point " + std::to_string(q.index) + " at tick " + std::to_string(p_tick));
					break;
				case net::DebugKind::SpawnSweep:
					if (sweep.active) {
						report(d.slot, q.id, false, "spawn sweep: one is already running");
						break;
					}
					sweep = Sweep();
					sweep.active = true;
					sweep.requester = d.slot;
					sweep.id = q.id;
					sweep.ticks = std::clamp<uint32_t>(q.ticks, 1u, kMaxSweepTicks);
					break;
				case net::DebugKind::SpawnTarget:
				case net::DebugKind::TargetInfo:
					sv_apply_debug(d, p_tick);
					break;
				default:
					break;
			}
		}
	}

	void run_sweep(sim::Tick p_tick) {
		if (!sweep.active) {
			return;
		}
		const std::vector<sim::SpawnPoint> &points = config.level->spawns;
		if (sweep.batch_active && p_tick - sweep.batch_start >= sweep.ticks) {
			for (size_t i = 0; i < slots.size(); ++i) {
				Slot &s = slots[i];
				if (!s.used || !s.dummy) {
					continue;
				}
				sim::SettleResult &r = sweep.results[s.sweep_entry];
				r.state = s.state;
				const sim::SpawnPoint &p = points[sweep.batch_points[s.sweep_entry]];
				sim::judge_settle(motor, *world, p, sim::SettleOptions(), r);
				net::SpawnSweepRow row;
				row.index = p.index;
				row.ok = r.ok;
				row.point = p.position;
				row.settled = r.state.position;
				row.lift = r.lift;
				row.shift = r.shift;
				row.gap = r.gap;
				row.grounded = (r.state.flags & sim::soldier_flags::kGrounded) != 0;
				row.overlapping = r.overlapping;
				sweep.rows.push_back(row);
				reset_slot(s);
			}
			sweep.batch_active = false;
		}
		if (!sweep.batch_active && sweep.next_point < points.size()) {
			sweep.results.clear();
			sweep.batch_points.clear();
			while (sweep.next_point < points.size()) {
				const int at = free_slot();
				if (at < 0) {
					break;
				}
				const sim::SpawnPoint &p = points[sweep.next_point];
				sim::SettleResult r = sim::place_spawn(motor, *world, p, 1, p_tick);
				if (!r.failure.empty()) {
					net::SpawnSweepRow row;
					row.index = p.index;
					row.ok = false;
					row.point = p.position;
					row.settled = r.placed;
					row.lift = r.lift;
					row.shift = r.shift;
					sweep.rows.push_back(row);
					++sweep.next_point;
					continue;
				}
				Slot &s = slots[size_t(at)];
				reset_slot(s);
				s.used = true;
				s.dummy = true;
				s.team = 0;
				s.spawned = true;
				s.state = r.state;
				s.sweep_entry = sweep.results.size();
				sweep.results.push_back(std::move(r));
				sweep.batch_points.push_back(uint32_t(sweep.next_point));
				++sweep.next_point;
			}
			if (!sweep.results.empty()) {
				sweep.batch_active = true;
				sweep.batch_start = p_tick;
			} else if (sweep.next_point < points.size()) {
				report(sweep.requester, sweep.id, false, "spawn sweep: no free slot");
				sweep = Sweep();
				return;
			}
		}
		if (!sweep.batch_active && sweep.next_point >= points.size()) {
			std::sort(sweep.rows.begin(), sweep.rows.end(), [](const net::SpawnSweepRow &a, const net::SpawnSweepRow &b) { return a.index < b.index; });
			size_t ok = 0;
			for (const net::SpawnSweepRow &r : sweep.rows) {
				ok += r.ok ? 1 : 0;
			}
			if (sweep.requester < slots.size() && slots[sweep.requester].used && slots[sweep.requester].remote) {
				const Slot &s = slots[sweep.requester];
				net::DebugReport rep;
				rep.id = sweep.id;
				rep.ok = ok == sweep.rows.size();
				rep.text = "spawn sweep: " + std::to_string(ok) + " / " + std::to_string(sweep.rows.size()) + " ok";
				rep.sweep = std::move(sweep.rows);
				send(s.peer, net::kChannelReliable, net::encode_debug_report(rep));
			}
			sweep = Sweep();
		}
	}

	void send_snapshots(sim::Tick p_tick, uint64_t p_now_ns) {
		const auto start = std::chrono::steady_clock::now();
		const uint64_t snapshots_before = snapshots_sent;
		sv_record_tick(p_tick);
		std::vector<std::pair<float, size_t>> order;
		for (size_t i = 0; i < slots.size(); ++i) {
			Slot &s = slots[i];
			if (!s.used || !s.remote || !s.hello) {
				continue;
			}
			net::Snapshot snap;
			snap.seq = s.snapshot_seq++;
			snap.server_tick = p_tick;
			snap.applied_sequence = s.applied_sequence;
			snap.applied_repeated = s.applied_repeated;
			const int64_t buffer = s.any_command ? int64_t(s.highest_sequence_tick) - int64_t(p_tick) : 0;
			snap.command_buffer_ticks = int8_t(std::clamp<int64_t>(buffer, -127, 127));
			snap.sequence_echo = uint16_t(s.highest_sequence & 0xFFFFu);
			if (s.any_command) {
				const uint64_t hold = p_now_ns > s.highest_sequence_ns ? (p_now_ns - s.highest_sequence_ns) / 100000u : 0u;
				snap.echo_hold_100us = uint16_t(std::min<uint64_t>(hold, 0xFFFFu));
			}
			snap.own_events = s.own_events;
			s.own_events = 0;
			if (s.spawned) {
				snap.own = s.state;
			}
			order.clear();
			const sim::Vec3 at = s.spawned ? s.state.position : sim::Vec3{};
			for (size_t j = 0; j < slots.size(); ++j) {
				const Slot &o = slots[j];
				if (j != i && o.used && o.spawned && alive(o.state)) {
					order.push_back({ distance2(at, o.state.position), j });
				}
			}
			std::sort(order.begin(), order.end());
			snap.others.reserve(order.size());
			for (const auto &[d, j] : order) {
				snap.others.push_back(net::quantize_other(uint8_t(j), slots[j].state));
			}
			sv_prepare_snapshot(i, snap);

			size_t dropped = 0;
			std::vector<uint8_t> bytes = net::encode_snapshot(snap, &dropped);
			sv_split_overflow(snap, dropped);
			if (bytes.empty()) {
				continue;
			}
			++snapshots_sent;
			snapshot_bytes += bytes.size();
			max_snapshot_bytes = std::max(max_snapshot_bytes, bytes.size());
			send(s.peer, net::kChannelUnreliable, std::move(bytes));
			sv_after_snapshot(i, snap);
		}
		sv_end_snapshots();
		last_encode_ms = ms_since(start);
		times.encode_ms = last_encode_ms;
		times.snapshots = uint32_t(snapshots_sent - snapshots_before);
	}

	void run_tick(std::vector<OutDatagram> &r_out) {
		const sim::Tick n = tick + 1u;
		const auto tick_start = std::chrono::steady_clock::now();
		times = GameServerTickTimes();
		times.tick = n;
		tick_spawns = 0;
		const uint64_t queries_before = world->query_count();
		const double paths_before = bots != nullptr ? bots->paths_ms_total() : 0.0;

		for (Slot &s : slots) {
			if (!s.used || !s.remote) {
				continue;
			}
			Buffered &b = s.buffer[n % s.buffer.size()];
			const bool have = b.valid && b.command.tick == n;
			sim::Command c;
			if (have) {
				c = b.command;
			}
			if (b.valid && b.command.tick <= n) {
				b.valid = false;
			}
			if (!s.spawned) {
				continue;
			}
			if (have) {
				s.last = c;
				s.applied_sequence = c.sequence;
				s.applied_repeated = false;
				s.last_applied_sequence = std::max(s.last_applied_sequence, c.sequence);
				++s.stats.commands_applied;
			} else {
				s.last = sim::repeated_command(s.last, n);
				s.applied_sequence = s.last.sequence;
				s.applied_repeated = true;
				++s.stats.commands_repeated;
			}
		}
		times.commands_ms = ms_since(tick_start);

		const auto sources_start = std::chrono::steady_clock::now();
		for (Slot &s : slots) {
			if (!s.used || s.remote || !s.spawned) {
				continue;
			}
			sim::Command c;
			if (s.dummy) {
				c = sim::idle_command(s.state.tick + 1u, s.state.yaw, s.state.pitch, s.state.target_pose);
			} else if (s.source) {
				c = s.source->command(n, s.state);
			}
			c.tick = n;
			c.flags = uint8_t(c.flags | sim::command_flags::kLocalSource);
			s.last = c;
			++s.stats.commands_applied;
		}
		times.sources_ms = ms_since(sources_start);
		times.paths_ms = bots != nullptr ? bots->paths_ms_total() - paths_before : 0.0;

		const auto step_start = std::chrono::steady_clock::now();
		for (Slot &s : slots) {
			if (!s.used || !s.spawned) {
				continue;
			}
			if (s.last.tick != s.state.tick + 1u) {
				++step_precondition_failures;
			}
			const bool was_alive = alive(s.state);
			const sim::SoldierState sv_before = s.state;
			sv_restrict(s);
			motor.step(s.state, s.last);
			++times.motor_steps;
			sv_step_soldier(s, sv_before, was_alive);
			if (was_alive && !alive(s.state) && !s.dummy) {
				const std::optional<uint32_t> respawn = sv_policy().respawn_tick(uint8_t(&s - slots.data()), n);
				s.want_spawn = respawn.has_value();
				s.respawn_at = respawn.value_or(0);
			}
		}
		last_step_ms = ms_since(step_start);
		times.step_ms = last_step_ms;
		motor_steps += times.motor_steps;
		const auto rules_start = std::chrono::steady_clock::now();

		sv_rules(n);
		for (size_t i = 0; i < slots.size(); ++i) {
			Slot &s = slots[i];
			if (s.used && !s.dummy && s.want_spawn && (!s.spawned || n >= s.respawn_at)) {
				spawn(i, SIZE_MAX, n);
			}
		}
		apply_debug(n);
		run_sweep(n);
		times.rules_ms = ms_since(rules_start);
		times.spawns = tick_spawns;
		tick = n;

		for (size_t i = 0; i < slots.size(); ++i) {
			Slot &s = slots[i];
			if (!s.used || !s.spawned) {
				continue;
			}
			s.own_events = uint16_t(s.own_events | s.state.events);
			if (s.tracing) {
				s.trace.push_back(s.state);
				sv_trace_tick(i);
				if (s.trace.size() >= s.trace_ticks) {
					if (s.remote) {
						net::DebugReport r;
						r.id = s.trace_id;
						r.ok = true;
						r.text = "trace of " + std::to_string(s.trace.size()) + " ticks from " + std::to_string(s.trace.front().tick);
						r.trace = std::move(s.trace);
						sv_trace_report(i, r);
						send(s.peer, net::kChannelReliable, net::encode_debug_report(r));
					}
					s.trace.clear();
					s.tracing = false;
				}
			}
		}
		if (config.snapshot_divisor <= 1 || n % config.snapshot_divisor == 0) {
			send_snapshots(n, net::monotonic_ns());
		}
		sv_send_rules(n);
		for (OutDatagram &d : pending_out) {
			r_out.push_back(std::move(d));
		}
		pending_out.clear();
		times.collision_queries = world->query_count() - queries_before;
		times.tick_ms = ms_since(tick_start);
	}

	int add_local(std::unique_ptr<CommandSource> p_source, int p_team, bool p_bot) {
		const int at = free_slot();
		if (at < 0 || p_source == nullptr) {
			return -1;
		}
		Slot &s = slots[size_t(at)];
		reset_slot(s);
		s.used = true;
		s.team = p_team;
		s.bot = p_bot;
		s.source = std::move(p_source);
		if (sv_rules_on()) {
			s.team = int(sv_connect(size_t(at), true, ""));
			if (s.team == 0) {
				sv_rules_leave(size_t(at));
				reset_slot(s);
				return -1;
			}
		}
		if (!spawn(size_t(at), SIZE_MAX, tick)) {
			reset_slot(s);
			return -1;
		}
		return at;
	}

	struct Sv;
	std::unique_ptr<Sv> sv;
	core::Status sv_init();
	void sv_ack(Slot &r_slot, const net::CommandPacket &p_packet);
	void sv_buffered(Slot &r_slot, const sim::Command &p_command);
	void sv_step_soldier(Slot &r_slot, const sim::SoldierState &p_before, bool p_was_alive);
	void sv_rules(sim::Tick p_tick);
	core::Status sv_place(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose, sim::Tick p_tick);
	void sv_on_spawn(size_t p_slot, sim::Tick p_tick);
	void sv_spawned(size_t p_slot, net::Spawned &r_message);
	void sv_apply_debug(const PendingDebug &p_debug, sim::Tick p_tick);
	void sv_record_tick(sim::Tick p_tick);
	void sv_prepare_snapshot(size_t p_slot, net::Snapshot &r_snapshot);
	void sv_split_overflow(net::Snapshot &r_snapshot, size_t p_dropped);
	void sv_after_snapshot(size_t p_slot, const net::Snapshot &p_snapshot);
	void sv_end_snapshots();
	void sv_match_info(size_t p_slot, net::MatchInfo &r_message);
	void sv_kit_select(int p_slot, std::span<const uint8_t> p_datagram);
	void sv_disconnect(size_t p_slot);
	void sv_trace_tick(size_t p_slot);
	void sv_trace_report(size_t p_slot, net::DebugReport &r_report);
	void sv_stats(GameServerStats &r_stats) const;

	RespawnPolicy &sv_policy();
	core::Status sv_revive(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, float p_health, sim::Tick p_tick);
	void sv_unspawn(size_t p_slot);
	void sv_orders(sim::Tick p_tick, std::span<const RulesOrder> p_orders);

	bool sv_rules_on() const;
	uint8_t sv_connect(size_t p_slot, bool p_bot, std::string_view p_name);
	void sv_rules_leave(size_t p_slot);
	bool sv_waits_for_deploy(size_t p_slot) const;
	void sv_restrict(Slot &r_slot);
	size_t sv_choose_spawn(size_t p_slot, const std::vector<sim::SpawnPoint> &p_points, sim::Tick p_tick);
	void sv_rules_match_info(net::MatchInfo &r_message) const;
	void sv_rules_join(size_t p_slot);
	void sv_send_rules(sim::Tick p_tick);
	void sv_spawn_request(int p_slot, std::span<const uint8_t> p_datagram);
};

core::Status GameServer::init(const GameServerConfig &p_config) {
	Impl &m = *impl_;
	if (p_config.level == nullptr || p_config.level->static_world == nullptr) {
		return core::make_error("GameServer: no level");
	}
	if (p_config.max_slots == 0 || p_config.max_slots > net::kMaxSlots) {
		return core::make_error("GameServer: max_slots must be 1.." + std::to_string(net::kMaxSlots));
	}
	if (p_config.snapshot_divisor == 0) {
		return core::make_error("GameServer: snapshot_divisor 0");
	}
	m.config = p_config;
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(p_config.level->static_world);
	if (!world) {
		return core::error_at("GameServer", world.error());
	}
	m.world = std::move(*world);
	BF3_TRY(m.motor.init(*m.world, p_config.level->motor));
	m.motor_hash = sim::motor_params_hash(p_config.level->motor);
	m.rng = p_config.seed;
	m.slots.resize(p_config.max_slots);
	for (Impl::Slot &s : m.slots) {
		m.reset_slot(s);
	}
	BF3_TRY(m.sv_init());
	for (uint32_t i = 0; i < p_config.walkers; ++i) {
		if (add_local_soldier(std::make_unique<Walker>(i), 2) < 0) {
			return core::make_error("GameServer: no slot for walker " + std::to_string(i));
		}
	}

	if (p_config.bots != nullptr && p_config.bots->count > 0) {
		core::Result<std::unique_ptr<BotDirector>> director = BotDirector::create(p_config.bots, p_config.seed);
		if (!director) {
			return core::error_at("GameServer", director.error());
		}
		m.bots = std::move(*director);
		for (uint32_t i = 0; i < m.bots->count(); ++i) {
			if (m.add_local(m.bots->source(i), 1 + int(i % 2u), true) < 0) {
				return core::make_error("GameServer: no slot for bot " + std::to_string(i));
			}
		}
	}
	BF3_TRY(add_firefight_bots(*this, p_config));
	if (p_config.tdm_bots != nullptr) {
		BF3_TRY(add_tdm_bots(*this, p_config));
	}
	return core::Status::success();
}

void GameServer::on_connect(uint32_t) {

}

void GameServer::on_disconnect(uint32_t p_peer) {
	Impl &m = *impl_;
	const int at = m.slot_of_peer(p_peer);
	if (at >= 0) {
		if (m.sweep.active && m.sweep.requester == uint32_t(at)) {
			m.sweep.requester = UINT32_MAX;
		}
		m.sv_disconnect(size_t(at));
		m.sv_rules_leave(size_t(at));
		m.reset_slot(m.slots[size_t(at)]);
	}
}

void GameServer::on_receive(uint32_t p_peer, uint8_t p_channel, std::span<const uint8_t> p_datagram) {
	Impl &m = *impl_;
	const std::optional<net::MessageType> type = net::peek_type(p_datagram);
	if (!type) {
		++m.bad_datagrams;
		return;
	}
	const int at = m.slot_of_peer(p_peer);
	switch (*type) {
		case net::MessageType::Hello: {
			core::Result<net::Hello> h = net::decode_hello(p_datagram);
			if (!h || p_channel != net::kChannelReliable) {
				++m.bad_datagrams;
				return;
			}
			m.handle_hello(p_peer, *h);
			return;
		}
		case net::MessageType::Ready: {
			core::Result<net::Ready> r = net::decode_ready(p_datagram);
			if (!r || at < 0) {
				++m.bad_datagrams;
				return;
			}
			if (r->source_digest && m.config.level->source_digest && *r->source_digest != *m.config.level->source_digest) {

				net::Kick k;
				k.reason = "level data differs from the server's (source digest)";
				m.send(p_peer, net::kChannelReliable, net::encode_kick(k));
				++m.level_mismatch_kicks;
				on_disconnect(p_peer);
				return;
			}
			Impl::Slot &s = m.slots[size_t(at)];
			if (!s.spawned && !m.sv_waits_for_deploy(size_t(at))) {
				s.want_spawn = true;
			}
			return;
		}
		case net::MessageType::CommandPacket: {
			core::Result<net::CommandPacket> c = net::decode_command_packet(p_datagram);
			if (!c || at < 0) {
				++m.bad_datagrams;
				return;
			}
			m.receive_commands(m.slots[size_t(at)], *c, net::monotonic_ns());
			return;
		}
		case net::MessageType::Loading: {
			if (!net::decode_loading(p_datagram) || at < 0) {
				++m.bad_datagrams;
			}
			return;
		}
		case net::MessageType::Bye:
			if (at >= 0) {
				on_disconnect(p_peer);
			}
			return;
		case net::MessageType::Debug: {
			core::Result<net::DebugRequest> d = net::decode_debug(p_datagram);
			if (!d || at < 0) {
				++m.bad_datagrams;
				return;
			}
			m.handle_debug(size_t(at), *d);
			return;
		}
		case net::MessageType::KitSelect:
			m.sv_kit_select(at, p_datagram);
			return;
		case net::MessageType::SpawnRequest:
			m.sv_spawn_request(at, p_datagram);
			return;
		default:
			++m.bad_datagrams;
			return;
	}
}

void GameServer::tick(std::vector<OutDatagram> &r_out) {
	impl_->run_tick(r_out);
}

sim::Tick GameServer::current_tick() const {
	return impl_->tick;
}

int GameServer::add_local_soldier(std::unique_ptr<CommandSource> p_source, int p_team) {
	return impl_->add_local(std::move(p_source), p_team, false);
}

GameServerStats GameServer::stats() const {
	const Impl &m = *impl_;
	GameServerStats g;
	g.tick = m.tick;
	g.snapshots_sent = m.snapshots_sent;
	g.snapshot_bytes = m.snapshot_bytes;
	g.max_snapshot_bytes = m.max_snapshot_bytes;
	g.bad_datagrams = m.bad_datagrams;
	g.spawn_failures = m.spawn_failures;
	g.level_mismatch_kicks = m.level_mismatch_kicks;
	g.rules_step_failures = m.rules_step_failures;
	g.step_precondition_failures = m.step_precondition_failures;
	g.last_step_ms = m.last_step_ms;
	g.last_encode_ms = m.last_encode_ms;

	g.last_tick_ms = m.times.tick_ms;
	g.last_commands_ms = m.times.commands_ms;
	g.last_sources_ms = m.times.sources_ms;
	g.last_rules_ms = m.times.rules_ms;
	g.motor_steps = m.motor_steps;
	g.collision_queries = m.world != nullptr ? m.world->query_count() : 0;
	if (m.bots != nullptr) {
		g.bots = m.bots->stats();
	}
	g.slots.resize(m.slots.size());
	for (size_t i = 0; i < m.slots.size(); ++i) {
		const Impl::Slot &s = m.slots[i];
		SlotStats &o = g.slots[i];
		o = s.stats;
		o.used = s.used;
		o.remote = s.remote;
		o.peer = s.peer;
		o.life = s.state.life;
		o.alive = s.spawned && alive(s.state);
		o.buffer_ticks = s.any_command ? int32_t(int64_t(s.highest_sequence_tick) - int64_t(m.tick)) : 0;
		o.state = s.state;
		g.clients += (s.used && s.remote) ? 1u : 0u;
		g.soldiers += o.alive ? 1u : 0u;
	}
	m.sv_stats(g);
	return g;
}

const GameServerTickTimes &GameServer::last_tick_times() const {
	return impl_->times;
}

const sim::SoldierState *GameServer::soldier(uint32_t p_slot) const {
	const Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.slots[p_slot].used || !m.slots[p_slot].spawned) {
		return nullptr;
	}
	return &m.slots[p_slot].state;
}

}

#include "bf3/host/target_bots.h"

#include <cstdlib>
#include <unordered_set>

namespace bf3::host {

struct GameServer::Impl::Sv {

	struct FixedRespawn final : RespawnPolicy {
		uint32_t ticks = 60;
		std::optional<uint32_t> respawn_tick(uint8_t, uint32_t p_death_tick) override { return p_death_tick + ticks; }
	};

	struct Sink final : CombatEventSink {
		Impl *m = nullptr;
		void hit(const net::HitEvent &p_event) override { m->sv->combat.hits.push_back(p_event); }
		void death(const net::DeathEvent &p_event) override {

			if (p_event.victim < m->slots.size()) {
				Slot &s = m->slots[p_event.victim];

				if (s.used && !s.dummy && !s.want_spawn) {
					const std::optional<uint32_t> at = m->sv_policy().respawn_tick(p_event.victim, m->tick + 1u);
					if (at) {
						s.want_spawn = true;
						s.respawn_at = *at;
					}
				}
			}
			net::DeathEvent e = p_event;
			TdmRules *tdm = m->sv->rules.get();
			if (tdm != nullptr) {
				e.respawn_tick = tdm->earliest_respawn(p_event.victim);
				e.revive_until = tdm->revive_until(p_event.victim);
				const bool fall = p_event.killer == p_event.victim || p_event.killer >= m->sv->tick_states.size();
				e.killer_health = fall ? 0.0f : m->sv->tick_states[p_event.killer].health;
			}
			m->sv->combat.deaths.push_back(e);
			if (tdm != nullptr) {
				tdm->death(e);
			}
		}
		void revive(const net::ReviveEvent &p_event) override {
			m->sv->combat.revives.push_back(p_event);
			if (m->sv->rules != nullptr) {
				m->sv->rules->revive(p_event);
			}
		}
		void detonation(const net::DetonationWire &p_event) override { m->sv->combat.detonations.push_back(p_event); }
	};
	struct View final : ServerView {
		Impl *m = nullptr;
		mutable std::vector<sim::SoldierState> cache;
		mutable sim::Tick cache_tick = 0;
		mutable bool cached = false;
		std::span<const sim::SoldierState> states() const override {
			if (!cached || cache_tick != m->tick) {
				cache.assign(m->slots.size(), sim::SoldierState());
				for (size_t i = 0; i < m->slots.size(); ++i) {
					if (m->slots[i].used && m->slots[i].spawned) {
						cache[i] = m->slots[i].state;
					}
				}
				cache_tick = m->tick;
				cached = true;
			}
			return cache;
		}
		uint8_t team(uint8_t p_slot) const override {
			return p_slot < m->slots.size() && m->slots[p_slot].used ? uint8_t(std::clamp(m->slots[p_slot].team, 0, 255)) : uint8_t(0);
		}
		bool alive(uint8_t p_slot) const override {
			return p_slot < m->slots.size() && m->slots[p_slot].used && m->slots[p_slot].spawned && bf3::host::alive(m->slots[p_slot].state);
		}
		const sim::weapons::SoldierWeaponsState &weapons(uint8_t p_slot) const override {
			static const sim::weapons::SoldierWeaponsState s_empty;
			return m->sv->gunplay != nullptr ? m->sv->gunplay->weapons(p_slot) : s_empty;
		}
		const sim::CollisionWorld &world() const override { return *m->world; }
		sim::Tick tick() const override { return m->tick + 1u; }

		std::span<const DownedSoldier> downed() const override {
			return m->sv->rules != nullptr ? m->sv->rules->downed() : ServerView::downed();
		}
		net::MatchPhase phase() const override {
			return m->sv->rules != nullptr ? m->sv->rules->phase() : ServerView::phase();
		}
		bool input_restricted() const override {
			return m->sv->rules != nullptr ? m->sv->rules->input_restricted() : ServerView::input_restricted();
		}
		uint8_t kit(uint8_t p_slot) const override {
			if (m->sv->rules == nullptr) {
				return ServerView::kit(p_slot);
			}
			const uint8_t k = m->sv->rules->kit_of(p_slot);
			return k != kKeepKit ? k : (m->sv->gunplay != nullptr ? m->sv->gunplay->kit(p_slot) : uint8_t(0));
		}
	};
	struct Placement {
		size_t slot = 0;
		sim::Vec3 position{};
		uint16_t yaw = 0;
		sim::Pose pose = sim::Pose::Stand;
	};
	struct PerSlot {
		uint8_t kit = 0;
		uint8_t next_team = 0;
		TargetBotSource *target = nullptr;
		std::vector<net::TraceGunplayTick> trace;
		uint32_t trace_dropped = 0;
		net::OtherSoldier record;
		bool record_valid = false;
	};

	std::unique_ptr<Gunplay> gunplay;
	std::unique_ptr<RespawnPolicy> respawn;
	Sink sink;
	View view;
	net::ShotQuantization frame;
	std::vector<PerSlot> per_slot;
	std::vector<Placement> placements;
	std::vector<sim::SoldierState> tick_states;
	net::CombatEvents combat;
	std::vector<net::OtherSoldier> records;
	std::vector<net::OtherSoldier> overflow;
	uint64_t main_mask = 0;
	std::vector<net::RemoteShot> remote;

	std::unique_ptr<TdmRules> rules;
	std::vector<RulesOrder> orders;
	sim::Tick player_list_tick = 0;
	bool player_list_sent = false;
};

GameServer::GameServer() :
		impl_(std::make_unique<Impl>()) {}

GameServer::~GameServer() = default;

core::Status GameServer::Impl::sv_init() {
	sv = std::make_unique<Sv>();
	Sv &v = *sv;
	v.sink.m = this;
	v.view.m = this;
	auto fixed = std::make_unique<Sv::FixedRespawn>();
	fixed->ticks = config.respawn_ticks;
	v.respawn = std::move(fixed);
	v.per_slot.resize(slots.size());
	v.frame = shot_frame(*config.level->static_world);
	const bool tdm = config.rules.mode == MatchRulesConfig::Mode::Tdm;
	if (tdm && (config.level->logic == nullptr || config.level->gunplay == nullptr)) {
		return core::make_error("GameServer: TDM rules need the level's logic graph and gunplay tables (SharedLoadOptions::tdm, ::gunplay)");
	}
	if (config.level->gunplay == nullptr) {
		return core::Status::success();
	}
	if (tdm) {
		config.gunplay.defib_def = config.level->gunplay->kits.defib_def;
	}
	const GunplayConfig &g = config.gunplay;
	if (!config.dev_commands && (g.exact_aim || g.infinite_magazine || g.target_bots > 0 || g.player_team != 0 || g.observer != nullptr)) {
		return core::make_error("GameServer: exact_aim, infinite_magazine, target bots, a player team and the observer are dev options (dev_commands)");
	}
	if (g.target_bots > slots.size()) {
		return core::make_error("GameServer: " + std::to_string(g.target_bots) + " target bots for " + std::to_string(slots.size()) + " slots");
	}
	core::Result<std::unique_ptr<Gunplay>> created = Gunplay::create(*config.level, g);
	if (!created) {
		return core::error_at("GameServer", created.error());
	}
	v.gunplay = std::move(*created);
	v.gunplay->set_world(*world);
	v.gunplay->set_match_seed(g.match_seed != 0 ? g.match_seed : derive_match_seed(config.seed, config.level->gunplay->level_hash));
	if (tdm) {
		core::Result<std::unique_ptr<TdmRules>> rules = TdmRules::create(*config.level, config.bots != nullptr ? config.bots->level : nullptr, config.rules,
				v.gunplay->match_seed(), uint32_t(slots.size()));
		if (!rules) {
			return core::error_at("GameServer", rules.error());
		}
		v.rules = std::move(*rules);
	}
	for (uint32_t i = 0; i < g.target_bots; ++i) {
		auto source = std::make_unique<TargetBotSource>(TargetBotConfig{});
		TargetBotSource *raw = source.get();
		const int team = g.target_team != 0 ? int(std::min<uint8_t>(g.target_team, net::kMaxTeam)) : (i % 2u == 0 ? 2 : 1);
		const int at = add_local(std::move(source), team, false);
		if (at < 0) {
			return core::make_error("GameServer: no slot for target bot " + std::to_string(i));
		}
		v.per_slot[size_t(at)].target = raw;
	}
	return core::Status::success();
}

void GameServer::Impl::sv_ack(Slot &r_slot, const net::CommandPacket &p_packet) {
	if (sv == nullptr || sv->gunplay == nullptr) {
		return;
	}
	const size_t i = size_t(&r_slot - slots.data());
	sv->gunplay->history().ack(uint8_t(i), p_packet.newest_snapshot, p_packet.snapshot_ack_bits, p_packet.others_ack_bits);
}

void GameServer::Impl::sv_buffered(Slot &r_slot, const sim::Command &p_command) {
	if (sv == nullptr || sv->gunplay == nullptr) {
		return;
	}
	const size_t i = size_t(&r_slot - slots.data());
	sv->gunplay->note_command(uint8_t(i), p_command, sv->gunplay->history().ack_state(uint8_t(i)));
}

void GameServer::Impl::sv_step_soldier(Slot &r_slot, const sim::SoldierState &p_before, bool p_was_alive) {
	if (sv == nullptr || sv->gunplay == nullptr || !p_was_alive || r_slot.dummy) {
		return;
	}
	const size_t i = size_t(&r_slot - slots.data());
	sv->gunplay->step_soldier(uint8_t(i), r_slot.last, p_before, r_slot.state);
}

void GameServer::Impl::sv_rules(sim::Tick p_tick) {
	if (sv == nullptr) {
		return;
	}
	Sv &v = *sv;
	if (v.gunplay != nullptr) {
		v.tick_states.assign(slots.size(), sim::SoldierState());
		for (size_t i = 0; i < slots.size(); ++i) {
			if (slots[i].used && slots[i].spawned && !slots[i].dummy) {
				v.tick_states[i] = slots[i].state;
			}
		}

		v.gunplay->step_projectiles(p_tick, v.tick_states);
		v.gunplay->apply_rules(p_tick, v.tick_states, v.sink);
		v.orders.clear();
		if (v.rules != nullptr) {
			const core::Status st = v.rules->step(p_tick, v.view, *v.gunplay, v.tick_states, v.orders);
			if (!st.ok()) {

				if (rules_step_failures++ == 0) {
					std::fprintf(stderr, "GameServer: tdm rules: %s\n", st.error().message.c_str());
				}
			}
		}
		for (size_t i = 0; i < slots.size(); ++i) {
			if (slots[i].used && slots[i].spawned && !slots[i].dummy) {
				slots[i].state = v.tick_states[i];
			}
		}
	}

	std::vector<Sv::Placement> placements = std::move(v.placements);
	v.placements.clear();
	for (const Sv::Placement &p : placements) {
		const core::Status st = sv_place(p.slot, p.position, p.yaw, p.pose, p_tick);
		if (!st.ok()) {
			std::fprintf(stderr, "GameServer: place_soldier: %s\n", st.error().message.c_str());
		}
	}
	if (v.rules != nullptr && !v.orders.empty()) {
		const std::vector<RulesOrder> orders = std::move(v.orders);
		v.orders.clear();
		sv_orders(p_tick, orders);
	}
}

core::Status GameServer::Impl::sv_place(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose, sim::Tick p_tick) {
	if (p_slot >= slots.size() || !slots[p_slot].used || slots[p_slot].dummy) {
		return core::make_error("place: no soldier slot " + std::to_string(p_slot));
	}
	Slot &s = slots[p_slot];
	if (s.remote) {
		return core::make_error("place: slot " + std::to_string(p_slot) + " is a client's");
	}
	if (!std::isfinite(p_position[0]) || !std::isfinite(p_position[1]) || !std::isfinite(p_position[2]) || size_t(p_pose) >= sim::kPoseCount) {
		return core::make_error("place: bad position or pose");
	}
	const bool was_alive = s.spawned && alive(s.state);

	s.state = motor.spawn_state(p_position, p_yaw, was_alive ? s.state.life : s.state.life + 1u, p_tick, false);
	s.state.pose = p_pose;
	s.state.target_pose = p_pose;
	s.spawned = true;
	s.want_spawn = false;
	s.last = sim::hold_command(s.state);
	if (sv->per_slot[p_slot].target != nullptr) {
		TargetBotConfig c;
		c.pose = p_pose;
		c.yaw = p_yaw;
		sv->per_slot[p_slot].target->set(c);
	}
	if (!was_alive) {
		s.state.events = uint16_t(s.state.events | sim::soldier_events::kSpawned);
		s.own_events = uint16_t(s.own_events | s.state.events);
		sv_on_spawn(p_slot, p_tick);
	}
	return core::Status::success();
}

void GameServer::Impl::sv_on_spawn(size_t p_slot, sim::Tick p_tick) {
	if (sv == nullptr || p_slot >= slots.size()) {
		return;
	}
	Slot &s = slots[p_slot];
	Sv::PerSlot &ps = sv->per_slot[p_slot];
	if (ps.next_team != 0) {
		s.team = ps.next_team;
		ps.next_team = 0;
	}
	if (sv->rules != nullptr) {
		s.team = sv->rules->team_of(uint8_t(p_slot));
		if (const uint8_t k = sv->rules->kit_of(uint8_t(p_slot)); k != kKeepKit) {
			ps.kit = k;
		}
		sv->rules->on_spawned(uint8_t(p_slot), p_tick);
	} else if (!s.remote) {
		ps.kit = uint8_t(p_slot % kKitCount);
	}
	if (sv->gunplay != nullptr && !s.dummy) {
		sv->gunplay->on_spawn(uint8_t(p_slot), ps.kit, uint8_t(std::clamp(s.team, 0, 255)), p_tick);
	}
}

void GameServer::Impl::sv_spawned(size_t p_slot, net::Spawned &r_message) {
	if (sv == nullptr || p_slot >= slots.size()) {
		return;
	}
	r_message.kit = sv->per_slot[p_slot].kit;
	r_message.team = uint8_t(std::clamp(slots[p_slot].team, 0, int(net::kMaxTeam)));
}

void GameServer::Impl::sv_apply_debug(const PendingDebug &p_debug, sim::Tick p_tick) {
	const net::DebugRequest &q = p_debug.request;
	if (q.kind == net::DebugKind::SpawnTarget) {
		const core::Status st = sv_place(q.slot, q.position, q.yaw, q.pose, p_tick);
		if (!st.ok()) {
			report(p_debug.slot, q.id, false, "spawn target: " + st.error().message);
			return;
		}
		char text[160];
		std::snprintf(text, sizeof(text), "target %u placed at (%.3f, %.3f, %.3f) yaw %u pose %u at tick %u", unsigned(q.slot), double(q.position[0]),
				double(q.position[1]), double(q.position[2]), unsigned(q.yaw), unsigned(q.pose), p_tick);
		report(p_debug.slot, q.id, true, text);
		return;
	}

	if (sv->gunplay == nullptr) {
		report(p_debug.slot, q.id, false, "target info: no gunplay on this server");
		return;
	}
	if (q.slot >= slots.size() || !slots[q.slot].used || !slots[q.slot].spawned) {
		report(p_debug.slot, q.id, false, "target info: no soldier in slot " + std::to_string(q.slot));
		return;
	}
	const std::optional<net::TargetInfoRow> row = sv->gunplay->target_info(q.slot, slots[q.slot].state);
	if (!row) {
		report(p_debug.slot, q.id, false, "target info: slot " + std::to_string(q.slot) + " is not a live armed soldier");
		return;
	}
	const Slot &requester = slots[p_debug.slot];
	if (!requester.used || !requester.remote || pending_out.size() >= kMaxPendingReplies) {
		return;
	}
	net::DebugReport r;
	r.id = q.id;
	r.ok = true;
	r.text = "target " + std::to_string(q.slot) + ": " + std::to_string(row->target.capsule_count) + " capsules at tick " + std::to_string(p_tick);
	r.targets.push_back(*row);
	send(requester.peer, net::kChannelReliable, net::encode_debug_report(r));
}

void GameServer::Impl::sv_record_tick(sim::Tick p_tick) {
	if (sv == nullptr || sv->gunplay == nullptr) {
		return;
	}
	Sv &v = *sv;
	v.records.clear();
	for (size_t i = 0; i < slots.size(); ++i) {
		Sv::PerSlot &ps = v.per_slot[i];
		ps.record_valid = false;
		const Slot &s = slots[i];
		if (s.used && s.spawned && alive(s.state)) {
			ps.record = v.gunplay->other_record(uint8_t(i), s.state);
			ps.record_valid = true;
			v.records.push_back(ps.record);
		}
	}

	v.gunplay->history().record_tick(p_tick, v.records);
}

void GameServer::Impl::sv_prepare_snapshot(size_t p_slot, net::Snapshot &r_snapshot) {
	if (sv == nullptr || sv->gunplay == nullptr) {
		return;
	}
	Sv &v = *sv;
	if (r_snapshot.own && v.gunplay->armed(uint8_t(p_slot))) {
		r_snapshot.own_weapons = net::weapons_wire_view(v.gunplay->weapons(uint8_t(p_slot)), r_snapshot.server_tick);
	}
	for (net::OtherSoldier &o : r_snapshot.others) {
		if (o.slot < v.per_slot.size() && v.per_slot[o.slot].record_valid) {
			o = v.per_slot[o.slot].record;
		}
	}
}

void GameServer::Impl::sv_split_overflow(net::Snapshot &r_snapshot, size_t p_dropped) {
	if (sv == nullptr || sv->gunplay == nullptr) {
		return;
	}
	Sv &v = *sv;

	v.overflow.clear();
	if (p_dropped > 0 && p_dropped <= r_snapshot.others.size()) {
		const size_t keep = r_snapshot.others.size() - p_dropped;
		v.overflow.assign(r_snapshot.others.begin() + ptrdiff_t(keep), r_snapshot.others.end());
		r_snapshot.others.resize(keep);
	}
	v.main_mask = 0;
	for (const net::OtherSoldier &o : r_snapshot.others) {
		if (o.slot < 64) {
			v.main_mask |= uint64_t(1) << o.slot;
		}
	}
}

void GameServer::Impl::sv_after_snapshot(size_t p_slot, const net::Snapshot &p_snapshot) {
	if (sv == nullptr || sv->gunplay == nullptr) {
		return;
	}
	Sv &v = *sv;
	Gunplay &g = *v.gunplay;
	const Slot &s = slots[p_slot];
	uint64_t others_mask = 0;
	if (!v.overflow.empty()) {
		net::SnapshotOthers so;
		so.seq = p_snapshot.seq;
		so.server_tick = p_snapshot.server_tick;
		so.others = v.overflow;
		size_t dropped = 0;
		std::vector<uint8_t> bytes = net::encode_snapshot_others(so, &dropped);
		g.count_others_dropped(dropped);
		if (!bytes.empty()) {
			const size_t sent = so.others.size() - std::min(dropped, so.others.size());
			for (size_t k = 0; k < sent; ++k) {
				if (so.others[k].slot < 64) {
					others_mask |= uint64_t(1) << so.others[k].slot;
				}
			}
			send(s.peer, net::kChannelUnreliable, std::move(bytes));
		}
	}

	g.history().sent(uint8_t(p_slot), p_snapshot.seq, p_snapshot.server_tick, v.main_mask, others_mask);

	v.remote.clear();
	for (const sim::weapons::ShotEvent &e : g.interval_shots()) {
		if (e.player_slot != p_slot) {
			v.remote.push_back(net::remote_shot_of(e));
		}
	}
	if (!v.remote.empty()) {
		sim::Vec3 eye = s.state.position;
		eye[1] += s.spawned ? motor.eye_height(s.state) : 0.0f;
		for (int part = 0; part < 2; ++part) {
			net::ShotEvents ev;
			ev.server_tick = p_snapshot.server_tick;
			ev.shots = v.remote;
			size_t dropped = 0;
			std::vector<uint8_t> bytes = net::encode_shot_events(ev, v.frame, eye, &dropped);
			if (dropped == 0 || part == 1 || bytes.empty()) {
				g.count_shot_events_dropped(dropped);
				if (!bytes.empty()) {
					send(s.peer, net::kChannelUnreliable, std::move(bytes));
				}
				break;
			}
			core::Result<net::ShotEvents> first = net::decode_shot_events(bytes, v.frame);
			send(s.peer, net::kChannelUnreliable, std::move(bytes));
			if (!first) {
				g.count_shot_events_dropped(dropped);
				break;
			}
			std::unordered_set<uint64_t> carried;
			for (const net::RemoteShot &r : first->shots) {
				carried.insert((uint64_t(r.slot) << 32) | r.shot_index);
			}
			std::vector<net::RemoteShot> rest;
			for (const net::RemoteShot &r : v.remote) {
				if (carried.find((uint64_t(r.slot) << 32) | r.shot_index) == carried.end()) {
					rest.push_back(r);
				}
			}
			if (rest.empty()) {
				break;
			}
			v.remote = std::move(rest);
		}
	}

	if (!v.combat.hits.empty() || !v.combat.deaths.empty() || !v.combat.detonations.empty() || !v.combat.revives.empty()) {
		net::CombatEvents c = v.combat;
		c.server_tick = p_snapshot.server_tick;
		size_t clamped = 0;
		std::vector<uint8_t> bytes = net::encode_combat_events(c, v.frame, &clamped);
		g.count_combat_clamped(clamped);
		if (!bytes.empty()) {
			send(s.peer, net::kChannelReliable, std::move(bytes));
		}
	}
}

void GameServer::Impl::sv_end_snapshots() {
	if (sv == nullptr) {
		return;
	}
	sv->combat = net::CombatEvents();
	if (sv->gunplay != nullptr) {
		sv->gunplay->end_snapshot_interval();
	}
}

void GameServer::Impl::sv_match_info(size_t p_slot, net::MatchInfo &r_message) {
	if (sv == nullptr || p_slot >= slots.size()) {
		return;
	}
	Sv::PerSlot &ps = sv->per_slot[p_slot];
	ps = Sv::PerSlot();
	if (sv->gunplay == nullptr) {
		return;
	}
	Gunplay &g = *sv->gunplay;
	ps.kit = g.config().player_kit < kKitCount ? g.config().player_kit : 0;
	if (config.dev_commands && (g.config().player_team == 1 || g.config().player_team == 2)) {
		slots[p_slot].team = g.config().player_team;
	}
	g.history().client_reset(uint8_t(p_slot));
	g.clear(uint8_t(p_slot));
	r_message.match_seed = g.match_seed();
	r_message.weapon_hash = g.weapon_hash();
	r_message.weapon_flags = g.weapon_flags();
	r_message.kit_hash = g.kit_hash();
	r_message.max_rewind_ticks = uint8_t(std::min<uint32_t>(g.config().lag.max_rewind_ticks, 255u));
}

void GameServer::Impl::sv_kit_select(int p_slot, std::span<const uint8_t> p_datagram) {
	core::Result<net::KitSelect> k = net::decode_kit_select(p_datagram);
	if (!k || p_slot < 0 || sv == nullptr || size_t(p_slot) >= slots.size()) {
		++bad_datagrams;
		if (sv != nullptr && sv->gunplay != nullptr) {
			sv->gunplay->count_kit_select(false, false);
		}
		return;
	}
	if (k->kit >= kKitCount || k->team > net::kMaxTeam) {

		if (sv->gunplay != nullptr) {
			sv->gunplay->count_kit_select(false, false);
		}
		return;
	}
	Sv::PerSlot &ps = sv->per_slot[size_t(p_slot)];
	bool team_refused = false;
	ps.kit = k->kit;
	if (k->team != 0) {
		if (config.dev_commands) {
			ps.next_team = k->team;
		} else {
			team_refused = true;
		}
	}
	if (sv->gunplay != nullptr) {
		sv->gunplay->count_kit_select(true, team_refused);
	}
}

void GameServer::Impl::sv_disconnect(size_t p_slot) {
	if (sv == nullptr || p_slot >= slots.size()) {
		return;
	}
	sv->per_slot[p_slot] = Sv::PerSlot();
	if (sv->gunplay != nullptr) {
		sv->gunplay->history().client_reset(uint8_t(p_slot));
		sv->gunplay->clear(uint8_t(p_slot));
	}
}

void GameServer::Impl::sv_trace_tick(size_t p_slot) {
	if (sv == nullptr || sv->gunplay == nullptr || p_slot >= slots.size()) {
		return;
	}
	Sv::PerSlot &ps = sv->per_slot[p_slot];
	if (slots[p_slot].trace.size() == 1) {
		ps.trace.clear();
		ps.trace_dropped = 0;
	}
	uint32_t dropped = 0;
	ps.trace.push_back(sv->gunplay->trace_tick(uint8_t(p_slot), tick, dropped));
	ps.trace_dropped += dropped;
	sv->gunplay->count_trace_dropped(dropped);
}

void GameServer::Impl::sv_trace_report(size_t p_slot, net::DebugReport &r_report) {
	if (sv == nullptr || sv->gunplay == nullptr || p_slot >= slots.size()) {
		return;
	}
	Sv::PerSlot &ps = sv->per_slot[p_slot];
	r_report.trace_gunplay = std::move(ps.trace);
	r_report.trace_dropped = ps.trace_dropped;
	ps.trace.clear();
	ps.trace_dropped = 0;
}

void GameServer::Impl::sv_stats(GameServerStats &r_stats) const {
	if (sv == nullptr || sv->gunplay == nullptr) {
		return;
	}
	const Gunplay &g = *sv->gunplay;
	r_stats.gunplay_active = true;
	r_stats.gunplay = g.stats();
	r_stats.match_seed = g.match_seed();
	r_stats.weapon_hash = g.weapon_hash();
	r_stats.kit_hash = g.kit_hash();
	r_stats.weapon_flags = g.weapon_flags();
}

core::Status GameServer::place_soldier(uint32_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose) {
	Impl &m = *impl_;
	if (m.sv == nullptr || p_slot >= m.slots.size() || !m.slots[p_slot].used || m.slots[p_slot].dummy) {
		return core::make_error("place_soldier: no soldier slot " + std::to_string(p_slot));
	}
	if (m.slots[p_slot].remote) {
		return core::make_error("place_soldier: slot " + std::to_string(p_slot) + " is a client's");
	}
	if (!std::isfinite(p_position[0]) || !std::isfinite(p_position[1]) || !std::isfinite(p_position[2]) || size_t(p_pose) >= sim::kPoseCount) {
		return core::make_error("place_soldier: non-finite position or bad pose");
	}
	if (m.sv->placements.size() >= m.slots.size() * 2) {
		return core::make_error("place_soldier: too many placements this tick");
	}
	m.sv->placements.push_back({ size_t(p_slot), p_position, p_yaw, p_pose });
	return core::Status::success();
}

const ServerView &GameServer::view() const {
	if (impl_->sv == nullptr) {

		std::fprintf(stderr, "GameServer::view() called before init\n");
		std::abort();
	}
	return impl_->sv->view;
}

const Gunplay *GameServer::gunplay() const {
	return impl_->sv != nullptr ? impl_->sv->gunplay.get() : nullptr;
}

RespawnPolicy &GameServer::Impl::sv_policy() {
	if (sv->rules != nullptr) {
		return *sv->rules;
	}
	return *sv->respawn;
}

bool GameServer::Impl::sv_rules_on() const {
	return sv != nullptr && sv->rules != nullptr;
}

uint8_t GameServer::Impl::sv_connect(size_t p_slot, bool p_bot, std::string_view p_name) {
	if (!sv_rules_on() || p_slot >= slots.size()) {
		return 1;
	}
	return sv->rules->on_connect(uint8_t(p_slot), p_bot, p_name);
}

void GameServer::Impl::sv_rules_leave(size_t p_slot) {
	if (sv_rules_on() && p_slot < slots.size()) {
		sv->rules->on_disconnect(uint8_t(p_slot));
	}
}

bool GameServer::Impl::sv_waits_for_deploy(size_t p_slot) const {
	if (!sv_rules_on() || p_slot >= slots.size()) {
		return false;
	}

	return config.rules.deploy_screen || sv->rules->team_of(uint8_t(p_slot)) == 0;
}

void GameServer::Impl::sv_restrict(Slot &r_slot) {
	if (sv_rules_on() && !r_slot.dummy) {
		r_slot.last = sv->rules->apply_input_restriction(r_slot.last);
	}
}

size_t GameServer::Impl::sv_choose_spawn(size_t p_slot, const std::vector<sim::SpawnPoint> &p_points, sim::Tick p_tick) {
	if (!sv_rules_on() || p_slot >= slots.size()) {
		return SIZE_MAX;
	}
	return sv->rules->choose_spawn(uint8_t(p_slot), p_points, sv->view, p_tick);
}

void GameServer::Impl::sv_rules_match_info(net::MatchInfo &r_message) const {
	if (!sv_rules_on()) {
		return;
	}
	r_message.phase = sv->rules->phase();
	r_message.logic_digest = config.level->logic_digest;
	r_message.rules_flags = uint8_t(net::rules_flags::kTdm | (config.rules.deploy_screen ? net::rules_flags::kDeployScreen : 0u) |
			(config.rules.revive ? net::rules_flags::kRevive : 0u));
}

void GameServer::Impl::sv_rules_join(size_t p_slot) {
	if (!sv_rules_on() || p_slot >= slots.size() || !slots[p_slot].remote) {
		return;
	}
	const uint32_t peer = slots[p_slot].peer;
	std::vector<uint8_t> state = net::encode_match_state(sv->rules->match_state());
	if (!state.empty()) {
		send(peer, net::kChannelReliable, std::move(state));
	}
	size_t clamped = 0;
	std::vector<uint8_t> list = net::encode_player_list(sv->rules->player_list(tick, true), &clamped);
	if (!list.empty()) {
		send(peer, net::kChannelReliable, std::move(list));
	}
}

void GameServer::Impl::sv_send_rules(sim::Tick p_tick) {
	if (!sv_rules_on()) {
		return;
	}
	Sv &v = *sv;
	TdmRules &rules = *v.rules;
	const bool state_changed = rules.match_state_changed();
	const bool list_due = rules.players_changed() && (!v.player_list_sent || p_tick - v.player_list_tick >= net::kPlayerListIntervalTicks);
	if (!state_changed && !list_due) {
		return;
	}
	std::vector<uint8_t> state;
	std::vector<uint8_t> list;
	if (state_changed) {
		state = net::encode_match_state(rules.match_state());
	}
	if (list_due) {
		size_t clamped = 0;
		list = net::encode_player_list(rules.player_list(p_tick, false), &clamped);
		v.player_list_tick = p_tick;
		v.player_list_sent = true;
	}
	for (const Slot &s : slots) {
		if (!s.used || !s.remote || !s.hello) {
			continue;
		}
		if (!state.empty()) {
			send(s.peer, net::kChannelReliable, state);
		}
		if (!list.empty()) {
			send(s.peer, net::kChannelReliable, list);
		}
	}
	if (state_changed) {
		rules.mark_state_sent();
	}
	if (list_due) {
		rules.mark_players_sent();
	}
}

void GameServer::Impl::sv_spawn_request(int p_slot, std::span<const uint8_t> p_datagram) {
	core::Result<net::SpawnRequest> r = net::decode_spawn_request(p_datagram);
	if (!r || p_slot < 0 || size_t(p_slot) >= slots.size() || !sv_rules_on()) {
		++bad_datagrams;
		return;
	}
	(void)sv->rules->request_spawn(uint8_t(p_slot), r->kit, r->spawn_choice, tick + 1u);
}

core::Status GameServer::Impl::sv_revive(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, float p_health, sim::Tick p_tick) {
	if (p_slot >= slots.size() || !slots[p_slot].used || slots[p_slot].dummy) {
		return core::make_error("revive: no soldier slot " + std::to_string(p_slot));
	}
	Slot &s = slots[p_slot];
	if (s.spawned && alive(s.state)) {
		return core::make_error("revive: slot " + std::to_string(p_slot) + " is alive");
	}
	if (!std::isfinite(p_position[0]) || !std::isfinite(p_position[1]) || !std::isfinite(p_position[2]) || !std::isfinite(p_health) || p_health <= 0.0f) {
		return core::make_error("revive: bad position or health");
	}

	s.state = motor.spawn_state(p_position, p_yaw, s.state.life + 1u, p_tick, false);
	s.state.health = std::min(p_health, s.state.health);
	s.state.events = uint16_t(s.state.events | sim::soldier_events::kSpawned);
	s.spawned = true;
	s.want_spawn = false;
	s.respawn_at = 0;
	s.last = sim::hold_command(s.state);
	s.own_events = uint16_t(s.own_events | s.state.events);
	sv_on_spawn(p_slot, p_tick);
	if (s.remote) {
		net::Spawned m;
		m.slot = uint8_t(p_slot);
		m.spawn_index = 0;
		m.state = s.state;
		m.revived = true;
		sv_spawned(p_slot, m);
		send(s.peer, net::kChannelReliable, net::encode_spawned(m));
	}
	return core::Status::success();
}

void GameServer::Impl::sv_unspawn(size_t p_slot) {
	if (p_slot >= slots.size() || !slots[p_slot].used || slots[p_slot].dummy) {
		return;
	}
	Slot &s = slots[p_slot];
	s.spawned = false;
	s.want_spawn = false;
	s.respawn_at = 0;
	if (sv != nullptr && sv->gunplay != nullptr) {
		sv->gunplay->clear(uint8_t(p_slot));
	}
}

void GameServer::Impl::sv_orders(sim::Tick p_tick, std::span<const RulesOrder> p_orders) {
	for (const RulesOrder &o : p_orders) {
		if (o.slot >= slots.size() || !slots[o.slot].used || slots[o.slot].dummy) {
			continue;
		}
		Slot &s = slots[o.slot];
		switch (o.kind) {
			case RulesOrder::Kind::Spawn:
				if (!s.spawned || !alive(s.state)) {
					spawn(o.slot, SIZE_MAX, p_tick);
				}
				break;
			case RulesOrder::Kind::Revive: {
				const core::Status st = sv_revive(o.slot, o.position, o.yaw, o.health, p_tick);
				if (!st.ok()) {
					std::fprintf(stderr, "GameServer: %s\n", st.error().message.c_str());
					break;
				}
				net::ReviveEvent e;
				e.reviver = o.reviver;
				e.revived = o.slot;
				e.tick = p_tick;
				sv->sink.revive(e);
				break;
			}
			case RulesOrder::Kind::Unspawn:
				sv_unspawn(o.slot);
				break;
		}
	}
}

const TdmRules *GameServer::rules() const {
	return impl_->sv != nullptr ? impl_->sv->rules.get() : nullptr;
}

}

namespace bf3::host {

int GameServer::add_local_bot(std::unique_ptr<CommandSource> p_source, int p_team) {
	return impl_->add_local(std::move(p_source), p_team, true);
}

}
