#pragma once
// GameServer: the M1 game on the authoritative server (plan §2.3 server row, §3.5, §3.7 spawns; topics/05 §4.1,
// §7; docs/design/m1.md §8). It runs inside ServerHost's thread (server_host.h): ServerHost owns the thread, the
// fixed 60 Hz schedule and the ENet host; GameServer is the per-tick logic and calls no Godot and no ENet API
// (it receives datagrams and returns datagrams), so tests drive it without sockets.
//
// Tick order (plan §2.3): ServerHost services ENet and hands every datagram to on_receive -> tick():
//   1. commands: per slot, the command for this tick from the slot's jitter buffer (by Command::tick); missing ->
//      sim::repeated_command of the slot's last command (edges cleared); commands for past ticks are dropped and
//      counted late. A command for a tick already buffered replaces it only when its sequence is newer (after a
//      client clock reset moved the command tick backwards), else it is a redundant copy and dropped. A new slot's
//      "last command" is sim::hold_command(spawn state), so the ticks between Spawned and the first real command
//      run idle at the spawn yaw, exactly as the client's catch-up predicts them (docs §8.2, §9.2)
//   2. command sources: bots / scripted walkers write their command for this tick (same Command, D11)
//   3. sim: every live soldier steps once (SoldierMotor over the server's CollisionWorld), slot order
//   4. rules: Ready / deaths -> (re)spawn at choose_spawn (after respawn_ticks for a death): sim::settle_spawn at
//      this tick N, so the state is stamped N with full protection; Spawned goes out at once and the soldier
//      steps from N + 1 (M1: fall damage is the only damage). Dev Teleport and SpawnAt requests received in
//      step 1 are applied here too, so their states are stamped N like a spawn and the motor's tick
//      precondition holds on the server (a Teleport keeps the life and has no protection, docs §6.8)
//   5. (lag-compensation history: M2a)
//   6. snapshots: every snapshot_divisor ticks, one Snapshot per connected client (own state + every other)
// Slots: 0..max_slots-1 for peers and server-side sources alike; a slot is the ghost id of its soldier.
// Spawn handoff assumptions (docs §8.2, the client side is client_session.h): a client sends no command before
// its first own state; a respawn does not clear the slot's jitter buffer, so commands the client sent for ticks
// after N while it was dead are applied from N + 1, which is what the client's catch-up assumes ("the ring
// command if the client made one"); the motor reads no Command::sequence or flags, so the server's kRepeated /
// kLocalSource marks never make its states differ from the client's.
// Events: SoldierState::events covers one tick and snapshots go out every snapshot_divisor ticks, so the server
// ORs each soldier's events per receiving client until its next snapshot (Snapshot::own_events).
// Dev SpawnSweep (net::DebugKind::SpawnSweep): place_spawn for a batch of dummies (as many as there are free
// slots; a 128-slot config takes all 102 Metro points in one batch, the default 64 takes two), idle commands over
// `ticks` live server ticks, judge_settle, rows back, dummies removed. Dummies are left out of choose_spawn's
// live-soldier list, so a respawn during the sweep still finds a point.
// Dev Trace (net::DebugKind::Trace): the requesting client's soldier state after every tick, for `ticks` ticks,
// returned in one DebugReport (E1 through the full path, docs §7.7, §14).
// Sending: ServerHost sends the returned OutDatagrams and flushes ENet in the same tick (server_host.h).

#include "bf3/core/result.h"
#include "bf3/host/level_load.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::host {

// A server-side command source (bots later, M1's scripted walkers and spawn-sweep dummies now): asked once per
// tick for the command of its slot. Runs on the server thread.
class CommandSource {
public:
	virtual ~CommandSource() = default;
	virtual sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) = 0;
};

struct GameServerConfig {
	std::shared_ptr<const SharedLevel> level; // static world, spawns, motor params
	uint8_t snapshot_divisor = 2; // 30 Hz snapshots at 60 Hz
	// Soldier slots, peers and server-side sources together (<= net::kMaxSlots). --m1-check and host.session use
	// net::kMaxSlots (128) so one SpawnSweep batch covers Metro's 102 points plus the player and the walkers.
	uint32_t max_slots = 64;
	uint32_t jitter_buffer_ticks = 32; // commands kept ahead of the server tick per slot
	uint32_t respawn_ticks = 60; // after death (M1 placeholder; the data's respawn base is unknown, plan §3.1)
	uint32_t walkers = 0; // scripted walkers circling near spawn points (interpolation targets, docs §8.3)
	bool dev_commands = false; // accept Debug requests (tests, --m1-check)
	uint64_t seed = 1; // spawn choice and walker scripts
};

struct OutDatagram {
	uint32_t peer = 0;
	uint8_t channel = 0;
	bool reliable = false;
	std::vector<uint8_t> bytes;
};

struct SlotStats {
	bool used = false;
	bool remote = false; // a peer (else a server-side source)
	uint32_t peer = 0;
	uint32_t life = 0;
	bool alive = false;
	uint64_t commands_applied = 0;
	uint64_t commands_repeated = 0;
	uint64_t commands_late = 0; // arrived after their tick ran
	uint64_t commands_replaced = 0; // a newer sequence replaced a buffered command of the same tick
	// The tick of the highest-sequence command received (not the highest tick: after a backward client clock reset
	// that would stay high for as many ticks as the jump) - the server tick. Snapshot::command_buffer_ticks.
	int32_t buffer_ticks = 0;
	// The authoritative state after the last tick, for tests and the HUD from other threads. E1 through the full
	// path is measured on Trace reports, not on this polled copy (docs §7.7).
	sim::SoldierState state;
};

struct GameServerStats {
	sim::Tick tick = 0;
	uint32_t clients = 0;
	uint32_t soldiers = 0; // live
	uint64_t snapshots_sent = 0;
	uint64_t snapshot_bytes = 0;
	size_t max_snapshot_bytes = 0;
	uint64_t bad_datagrams = 0; // failed to decode (counted, ignored)
	// Motor steps whose command tick != state tick + 1 (a spawn, Teleport or SpawnAt not stamped at its rules tick
	// would show here; must stay 0, host.session.unit). Added in T4 (docs §8.4).
	uint64_t step_precondition_failures = 0;
	double last_step_ms = 0; // motor steps of the last tick
	double last_encode_ms = 0;
	std::vector<SlotStats> slots;
};

class GameServer {
public:
	GameServer();
	~GameServer();
	GameServer(const GameServer &) = delete;
	GameServer &operator=(const GameServer &) = delete;

	// Builds the server's CollisionWorld over the shared static shapes, the motor and the walkers.
	core::Status init(const GameServerConfig &p_config);
	void on_connect(uint32_t p_peer);
	void on_disconnect(uint32_t p_peer);
	void on_receive(uint32_t p_peer, uint8_t p_channel, std::span<const uint8_t> p_datagram);
	// Runs one tick (the order above) and appends what to send to r_out.
	void tick(std::vector<OutDatagram> &r_out);
	sim::Tick current_tick() const;

	// A server-side soldier (bot / walker): takes a free slot, spawns at choose_spawn. Returns the slot or -1.
	int add_local_soldier(std::unique_ptr<CommandSource> p_source, int p_team);
	GameServerStats stats() const; // server thread; ServerHost publishes a copy for other threads
	// The authoritative state of a slot (server thread; tests).
	const sim::SoldierState *soldier(uint32_t p_slot) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::host
