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

#include <cstdint>
#include <type_traits>
#include <vector>

namespace bf3::sim {

class SoldierMotor;

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

private:
	std::vector<PredictionEntry> slots_;
	std::vector<uint8_t> used_;
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
