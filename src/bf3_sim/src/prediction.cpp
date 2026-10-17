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

// M2a cl (docs/design/m2a.md §8.2; as built: docs/design/m2a-cl.md): the weapon lane and catch_up's weapon replay.
#include "bf3/sim/weapons/weapon_rules.h"

namespace bf3::sim {

void PredictionRing::attach_weapons(WeaponLane *p_lane) {
	weapons_ = p_lane;
}

WeaponLane *PredictionRing::weapons() const {
	return weapons_;
}

WeaponLane::WeaponLane(uint32_t p_capacity, std::shared_ptr<const weapons::WeaponTable> p_table, const MotorParams &p_motor, uint64_t p_match_seed) :
		table_(std::move(p_table)),
		motor_(p_motor),
		match_seed_(p_match_seed),
		slots_(std::max<uint32_t>(p_capacity, 1u)),
		used_(std::max<uint32_t>(p_capacity, 1u), 0) {}

uint32_t WeaponLane::capacity() const {
	return uint32_t(slots_.size());
}

const WeaponPredictionEntry *WeaponLane::find(Tick p_tick) const {
	const size_t at = size_t(p_tick % capacity());
	if (used_[at] == 0 || slots_[at].tick != p_tick) {
		return nullptr;
	}
	return &slots_[at];
}

const WeaponPredictionEntry *WeaponLane::newest_at_or_below(Tick p_tick) const {
	const uint32_t n = capacity();
	for (uint32_t back = 0; back < n && back <= p_tick; ++back) {
		if (const WeaponPredictionEntry *e = find(p_tick - back)) {
			return e;
		}
	}
	return nullptr;
}

void WeaponLane::put(const WeaponPredictionEntry &p_entry) {
	const size_t at = size_t(p_entry.tick % capacity());
	slots_[at] = p_entry;
	slots_[at].state.tick = p_entry.tick;
	used_[at] = 1;
}

void WeaponLane::clear() {
	std::fill(used_.begin(), used_.end(), uint8_t(0));
}

WeaponPredictionEntry WeaponLane::step(const WeaponPredictionEntry &p_from, const Command &p_command, const SoldierState &p_before,
		const SoldierState &p_after) {
	WeaponPredictionEntry out = p_from;
	out.tick = p_command.tick;
	if (p_from.tick + 1u != p_command.tick) {
		++stats_.gaps; // a base older than the tick before (never on the client's paths): stepped as if idle until now
		out.state.tick = p_command.tick > 0 ? p_command.tick - 1u : 0u;
	}
	weapons::WeaponTickOutput output;
	if (p_before.life == 0 || p_before.mode == MoveMode::Dead || table_ == nullptr) {
		// Dead before this tick: no weapon step (m2a.md §4.1 step 4a), the state carried to the tick.
		out.state.tick = p_command.tick;
		out.state.events = 0;
		++stats_.dead_ticks;
	} else {
		const weapons::WeaponContext context = weapons::make_weapon_context(motor_, p_before, p_after, p_command);
		weapons::step_weapons_m2a(*table_, out.state, p_command, context, match_seed_, p_from.spawn_tick, output);
		++stats_.steps;
		out.state.tick = p_command.tick;
	}
	if (p_command.tick > emitted_through_) {
		emitted_through_ = p_command.tick;
		++stats_.emitted_ticks;
		if (sink_ != nullptr) {
			sink_->emitted(p_command, output, out);
		}
	} else {
		++stats_.replayed_ticks;
	}
	return out;
}

void WeaponLane::set_sink(WeaponEmitSink *p_sink) {
	sink_ = p_sink;
}

Tick WeaponLane::emitted_through() const {
	return emitted_through_;
}

void WeaponLane::set_emitted_through(Tick p_tick) {
	emitted_through_ = p_tick;
}

const weapons::WeaponTable &WeaponLane::table() const {
	return *table_;
}

std::shared_ptr<const weapons::WeaponTable> WeaponLane::table_ptr() const {
	return table_;
}

uint64_t WeaponLane::match_seed() const {
	return match_seed_;
}

const WeaponLaneStats &WeaponLane::stats() const {
	return stats_;
}

WeaponReplay::WeaponReplay(WeaponLane *p_lane, const SoldierState &p_base) :
		lane_(p_lane) {
	if (lane_ == nullptr) {
		return;
	}
	const WeaponPredictionEntry *e = lane_->newest_at_or_below(p_base.tick);
	if (e == nullptr) {
		return;
	}
	current_ = *e;
	if (current_.tick != p_base.tick) {
		// catch_up's long-gap skip re-stamped the base: the weapons wait there too (idle, as the motor).
		current_.tick = p_base.tick;
		current_.state.tick = p_base.tick;
		lane_->put(current_);
	}
	active_ = true;
}

void WeaponReplay::step(const Command &p_command, const SoldierState &p_before, const SoldierState &p_after) {
	if (!active_) {
		return;
	}
	current_ = lane_->step(current_, p_command, p_before, p_after);
	lane_->put(current_);
}

} // namespace bf3::sim
