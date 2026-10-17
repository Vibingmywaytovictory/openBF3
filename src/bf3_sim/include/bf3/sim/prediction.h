#pragma once
// Client-side prediction state and the one catch-up rule (plan §3.5 "prediction / reconciliation"; topics/05
// §4.1; docs/design/m1.md §9.2). Godot-free and network-free: a pure function over the motor, so sim.motor.unit
// (T2) tests the exact code ClientSession (bf3_host, T4) runs, against the server's own stepping of the same
// commands.
//
// The ring is keyed by tick: entry t holds the command tick t was predicted with and the predicted state after it
// (state.tick == command.tick == t). The client predicts tick C from the entry at C - 1, so it always needs that
// entry. catch_up is the only way states enter the ring other than the per-tick prediction; it runs
//   - when the newest snapshot's own state S (tick T) differs from the ring's entry at T, when the ring has no
//     entry at T (a stall, a clock reset), or when S has a newer life (a spawn): catch_up(S, C);
//   - when the ring has no entry at C - 1 (a forward clock reset, a stall): catch_up(base, C) with base = the
//     ring's newest entry at or below C - 1, or the newest authoritative state when that is newer.
// The rule, from base state B at tick b to p_until = C:
//   prev = the ring's command at b if held, else hold_command(B);  ring[b] = (prev, B, sent as it was)
//   for t = b + 1 .. C - 1:
//       cmd   = the ring's command at t if the client made and sent one for t (PredictionEntry::sent)
//               else repeated_command(prev, t)       // what the server runs for a tick it got no command for
//       state = step(state, cmd)                     // cmd.tick == state.tick + 1 by construction
//       ring[t] = (cmd, state, sent kept); prev = cmd
// Synthesised commands are never sent (the CommandPacket's redundant commands come from the client's own send
// history), and a later catch-up synthesises them again from its own prev, since a correction can change it.

#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/soldier_motor.h" // M2a cl: MotorParams (the weapon context)
#include "bf3/sim/weapons/weapon_sim.h" // M2a cl

#include <cstdint>
#include <memory> // M2a cl
#include <type_traits>
#include <vector>

namespace bf3::sim {

class SoldierMotor;
class WeaponLane; // M2a cl (below)

struct PredictionEntry {
	Command command; // the command state was predicted with (command.tick == state.tick)
	SoldierState state; // the predicted (or, at a catch-up's base tick, authoritative) state after command.tick
	bool sent = false; // the client made and sent this command; false: synthesised by a catch-up (never sent)
};
static_assert(std::is_trivially_copyable_v<PredictionEntry>);

// A fixed ring of p_capacity ticks (ClientSessionConfig::prediction_ring, 128 = 2.1 s). A slot holds the entry of
// one tick; find() checks the stored tick, so an entry older than the capacity is never returned for a newer tick.
class PredictionRing {
public:
	explicit PredictionRing(uint32_t p_capacity = 128);
	uint32_t capacity() const;
	// The entry of exactly p_tick, nullptr when not held (never predicted, overwritten, discarded).
	const PredictionEntry *find(Tick p_tick) const;
	// The held entry with the highest tick <= p_tick, nullptr when none (searches at most capacity ticks back).
	const PredictionEntry *newest_at_or_below(Tick p_tick) const;
	// Stores p_entry at p_entry.state.tick, replacing whatever the slot held.
	void put(const PredictionEntry &p_entry);
	// Drops every entry with a tick above p_tick (a backward clock reset to C: discard_after(C - 1)).
	void discard_after(Tick p_tick);
	void clear();
	// M2a cl (docs/design/m2a.md §8.2): the weapons replayed beside the motor. catch_up steps the attached lane after
	// every motor step (null: the motor alone, M1). The lane keys its entries by tick like the ring and is owned by
	// the caller.
	void attach_weapons(WeaponLane *p_lane);
	WeaponLane *weapons() const;

private:
	std::vector<PredictionEntry> slots_;
	std::vector<uint8_t> used_;
	WeaponLane *weapons_ = nullptr; // M2a cl
};

struct CatchUpStats {
	uint32_t steps = 0; // motor steps run (C - 1 - base tick)
	uint32_t synthesized = 0; // of them, ticks without a sent command (repeats of prev)
};

// The rule above: brings r_ring to hold an entry for every tick p_base.tick .. p_until - 1, starting from p_base,
// so the next prediction (tick p_until) steps from r_ring.find(p_until - 1). Requires p_base.tick < p_until (the
// caller forces its clock forward to at least p_base.tick + 1 first, docs §9.2); with p_base.tick == p_until - 1 it
// only stores the base entry. p_motor must be bound to the client's CollisionWorld. Caller's thread.
CatchUpStats catch_up(const SoldierMotor &p_motor, const SoldierState &p_base, PredictionRing &r_ring, Tick p_until);

} // namespace bf3::sim

// M2a cl (docs/design/m2a.md §8.2; as built: docs/design/m2a-cl.md) ----------------------------------------------
// The own soldier's weapons in the prediction: one SoldierWeaponsState per predicted tick beside the ring's motor
// entry, stepped with the same commands in the same order as the server (§4.1 step 4a) through
// weapons::step_weapons_m2a (the two §5.2 rules), with the life's spawn tick kept in the entry. The client steps it
// for every predicted tick (ClientSession), and catch_up steps it after every motor step of a replay (the named
// insertion in prediction.cpp), so a reconciliation replays the weapons too.
//
// Emission (wp §7.2, m2a.md §8.2 "only the first prediction of a tick emits"): the lane remembers the highest tick
// it has emitted. A step of a newer tick hands its shots and timed events to the sink once; a replay of a tick at or
// below that keeps the state and discards its events, so effects and sounds never fire twice. The client sets the
// mark to the spawn tick on a new life (and leaves it on a backward clock reset: those ticks were emitted).
// A soldier dead before the tick steps no weapon (the server's rule, §4.1 step 4a): the state is carried to the tick
// unchanged with no events.
namespace bf3::sim {

struct WeaponPredictionEntry {
	Tick tick = 0; // the tick the state is the result of (state.tick)
	Tick spawn_tick = 0; // the life's spawn tick (step_weapons_m2a's fire-mode rule at spawn_tick + 1)
	weapons::SoldierWeaponsState state;
};
static_assert(std::is_trivially_copyable_v<WeaponPredictionEntry>);

// Receives the output of the first prediction of each tick (client thread).
class WeaponEmitSink {
public:
	virtual ~WeaponEmitSink() = default;
	virtual void emitted(const Command &p_command, const weapons::WeaponTickOutput &p_output, const WeaponPredictionEntry &p_after) = 0;
};

struct WeaponLaneStats {
	uint64_t steps = 0; // weapon steps run (first predictions and replays)
	uint64_t emitted_ticks = 0; // ticks handed to the sink
	uint64_t replayed_ticks = 0; // steps of ticks at or below the emitted mark (events discarded)
	uint64_t dead_ticks = 0; // ticks carried without a step (the soldier was dead before them)
	uint64_t gaps = 0; // steps whose base was older than command tick - 1 (re-stamped; must stay 0)
};

class WeaponLane {
public:
	WeaponLane(uint32_t p_capacity, std::shared_ptr<const weapons::WeaponTable> p_table, const MotorParams &p_motor, uint64_t p_match_seed);

	uint32_t capacity() const;
	const WeaponPredictionEntry *find(Tick p_tick) const;
	const WeaponPredictionEntry *newest_at_or_below(Tick p_tick) const;
	void put(const WeaponPredictionEntry &p_entry);
	void clear();

	// One tick: the weapons after p_command from p_from (normally the entry of p_command.tick - 1), the motor having
	// gone p_before -> p_after with the same command. Emits through the sink when p_command.tick is above the
	// emitted mark. Does not store the result (the caller puts it where the motor entry goes).
	WeaponPredictionEntry step(const WeaponPredictionEntry &p_from, const Command &p_command, const SoldierState &p_before, const SoldierState &p_after);

	void set_sink(WeaponEmitSink *p_sink);
	Tick emitted_through() const;
	void set_emitted_through(Tick p_tick);
	const weapons::WeaponTable &table() const;
	std::shared_ptr<const weapons::WeaponTable> table_ptr() const;
	uint64_t match_seed() const;
	const WeaponLaneStats &stats() const;

private:
	std::shared_ptr<const weapons::WeaponTable> table_;
	MotorParams motor_;
	uint64_t match_seed_ = 0;
	std::vector<WeaponPredictionEntry> slots_;
	std::vector<uint8_t> used_;
	WeaponEmitSink *sink_ = nullptr;
	Tick emitted_through_ = 0;
	WeaponLaneStats stats_;
};

// catch_up's weapon side (the named insertion): starts from the lane's entry at the base tick (or the newest below
// it, re-stamped), steps after every motor step and stores what it steps. Inert without a lane or a base entry.
class WeaponReplay {
public:
	WeaponReplay(WeaponLane *p_lane, const SoldierState &p_base);
	void step(const Command &p_command, const SoldierState &p_before, const SoldierState &p_after);

private:
	WeaponLane *lane_ = nullptr;
	bool active_ = false;
	WeaponPredictionEntry current_;
};

} // namespace bf3::sim
