#pragma once
// GameServer: the M1 game on the authoritative server (plan §2.3 server row, §3.5, §3.7 spawns; topics/05 §4.1,
// §7; docs/design/m1.md §8). It runs inside ServerHost's thread (server_host.h): ServerHost owns the thread, the
// fixed 60 Hz schedule and the ENet host; GameServer is the per-tick logic and calls no Godot and no ENet API
// (it receives datagrams and returns datagrams), so tests drive it without sockets.
//
// Tick order (plan §2.3): ServerHost services ENet and hands every datagram to on_receive -> tick():
//   1. commands: per slot, the command for this tick from the slot's jitter buffer (by Command::tick); missing ->
//      sim::repeated_command (edges cleared); commands for past ticks are dropped and counted late
//   2. command sources: bots / scripted walkers write their command for this tick (same Command, D11)
//   3. sim: every live soldier steps once (SoldierMotor over the server's CollisionWorld), slot order
//   4. rules: deaths -> respawn after respawn_ticks at choose_spawn (M1: fall damage is the only damage)
//   5. (lag-compensation history: M2a)
//   6. snapshots: every snapshot_divisor ticks, one Snapshot per connected client (own state + every other)
// Slots: 0..max_slots-1 for peers and server-side sources alike; a slot is the ghost id of its soldier.

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
	uint32_t max_slots = 64;
	uint32_t jitter_buffer_ticks = 32; // commands kept ahead of the server tick per slot
	uint32_t respawn_ticks = 60; // after death (M1 placeholder; the data's respawn base is unknown, plan §3.1)
	uint32_t walkers = 0; // scripted walkers circling near spawn points (interpolation targets, docs §8.4)
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
	int32_t buffer_ticks = 0; // newest command tick received - server tick
};

struct GameServerStats {
	sim::Tick tick = 0;
	uint32_t clients = 0;
	uint32_t soldiers = 0; // live
	uint64_t snapshots_sent = 0;
	uint64_t snapshot_bytes = 0;
	size_t max_snapshot_bytes = 0;
	uint64_t bad_datagrams = 0; // failed to decode (counted, ignored)
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
