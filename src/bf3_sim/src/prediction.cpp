// PredictionRing and sim::catch_up, the one catch-up rule of the client's prediction (docs/design/m1.md §9.2; the
// rule itself is written out in prediction.h). Godot-free, network-free; ClientSession (T4) calls it on its thread.
#include "bf3/sim/prediction.h"

#include "bf3/sim/soldier_motor.h"

#include <algorithm>

namespace bf3::sim {

namespace {

// A catch-up never runs more motor steps than this (one minute of ticks). A gap that long only comes from a
// damaged or hostile authoritative tick, or a client thread stalled for a minute; the excess is skipped as if the
// soldier had stood still (the base state re-stamped at the later tick), and the next snapshot corrects whatever
// that got wrong. Without the cap a snapshot naming tick 2^32 - 1 would make the client step for hours.
constexpr uint32_t kMaxCatchUpSteps = 60u * kSimHz;

} // namespace

PredictionRing::PredictionRing(uint32_t p_capacity) :
		slots_(std::max<uint32_t>(p_capacity, 1u)), used_(std::max<uint32_t>(p_capacity, 1u), 0) {}

uint32_t PredictionRing::capacity() const {
	return uint32_t(slots_.size());
}

const PredictionEntry *PredictionRing::find(Tick p_tick) const {
	const size_t at = size_t(p_tick % capacity());
	if (used_[at] == 0 || slots_[at].state.tick != p_tick) {
		return nullptr;
	}
	return &slots_[at];
}

const PredictionEntry *PredictionRing::newest_at_or_below(Tick p_tick) const {
	const uint32_t n = capacity();
	for (uint32_t back = 0; back < n; ++back) {
		if (back > p_tick) {
			break; // no tick below 0
		}
		if (const PredictionEntry *e = find(p_tick - back)) {
			return e;
		}
	}
	return nullptr;
}

void PredictionRing::put(const PredictionEntry &p_entry) {
	const size_t at = size_t(p_entry.state.tick % capacity());
	slots_[at] = p_entry;
	used_[at] = 1;
}

void PredictionRing::discard_after(Tick p_tick) {
	for (size_t i = 0; i < slots_.size(); ++i) {
		if (used_[i] != 0 && slots_[i].state.tick > p_tick) {
			used_[i] = 0;
		}
	}
}

void PredictionRing::clear() {
	std::fill(used_.begin(), used_.end(), uint8_t(0));
}

CatchUpStats catch_up(const SoldierMotor &p_motor, const SoldierState &p_base, PredictionRing &r_ring, Tick p_until) {
	CatchUpStats stats;
	SoldierState state = p_base;
	// The base entry: the ring's command at the base tick when it holds one (what the client predicted that tick
	// with, sent or synthesised), else the hold command of the base state.
	Command prev = hold_command(p_base);
	bool base_sent = false;
	if (const PredictionEntry *e = r_ring.find(p_base.tick)) {
		prev = e->command;
		prev.tick = p_base.tick;
		base_sent = e->sent;
	}
	if (p_until <= p_base.tick) {
		// Precondition broken by the caller (its clock must be past the base): store the base only.
		r_ring.put({ prev, state, base_sent });
		return stats;
	}
	if (p_until - 1u - p_base.tick > kMaxCatchUpSteps) {
		state.tick = p_until - 1u - kMaxCatchUpSteps;
		prev = repeated_command(prev, state.tick);
		base_sent = false;
	}
	// Only ticks the ring still holds when the catch-up ends are stored: an older tick's slot is the slot of a newer
	// tick of the range, whose sent command must still be there when the loop reaches it.
	const uint32_t capacity = r_ring.capacity();
	const auto survives = [&](Tick p_tick) { return p_until - p_tick <= capacity; };
	if (survives(state.tick)) {
		r_ring.put({ prev, state, base_sent });
	}
	for (Tick t = state.tick + 1u; t < p_until; ++t) {
		const PredictionEntry *e = r_ring.find(t);
		const bool sent = e != nullptr && e->sent && e->command.tick == t;
		const Command cmd = sent ? e->command : repeated_command(prev, t);
		p_motor.step(state, cmd);
		if (survives(t)) {
			r_ring.put({ cmd, state, sent });
		}
		prev = cmd;
		++stats.steps;
		stats.synthesized += sent ? 0u : 1u;
	}
	return stats;
}

} // namespace bf3::sim
