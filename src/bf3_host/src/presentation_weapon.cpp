#include "bf3/host/presentation_weapon.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace sw = sim::weapons;

namespace {

const net::WeaponTimersWire *active_timers(const net::WeaponsWire &p_wire) {
	return p_wire.timers[0].slot == p_wire.active ? &p_wire.timers[0] : nullptr;
}

double seconds_at(int32_t p_offset, sim::Tick p_tick) {
	if (p_offset == net::kWeaponsWireTimeZero) {
		return 0.0;
	}
	return sw::units_to_seconds(sw::tick_start(p_tick) + sw::SimTime(p_offset));
}

}

PresentedWeapon present_weapon(const net::WeaponsWire &p_prev, const net::WeaponsWire &p_curr, sim::Tick p_curr_tick, double p_fraction,
		const sw::WeaponTable &p_table) {
	PresentedWeapon p;
	if (p_curr.active >= sw::kMaxWeaponSlots) {
		return p;
	}
	const net::WeaponSlotWire &slot = p_curr.slots[p_curr.active];
	const net::WeaponTimersWire *b = active_timers(p_curr);
	if (slot.def == sw::kNoWeapon || slot.def >= p_table.defs.size() || b == nullptr) {
		return p;
	}
	const double f = std::clamp(std::isfinite(p_fraction) ? p_fraction : 0.0, 0.0, 1.0);
	p.valid = true;
	p.def = slot.def;
	p.active_slot = p_curr.active;
	p.pending_slot = p_curr.pending;
	p.phase = b->phase;
	p.mode = slot.mode;
	p.flags = b->flags;
	p.magazine = slot.magazine;
	p.reserve = slot.reserve;
	p.magazine_capacity = p_table.defs[slot.def].magazine_capacity;
	const sim::Tick prev_tick = p_curr_tick > 0 ? p_curr_tick - 1u : 0u;
	p.time_s = sw::units_to_seconds(sw::tick_end(prev_tick)) + f * sw::units_to_seconds(sw::kTimeUnitsPerTick);
	p.phase_start_s = seconds_at(b->phase_start, p_curr_tick);
	p.phase_end_s = seconds_at(b->phase_end, p_curr_tick);
	const double length = p.phase_end_s - p.phase_start_s;
	if (b->phase_end != net::kWeaponsWireTimeZero && length > 0.0) {
		p.phase_progress = float(std::clamp((p.time_s - p.phase_start_s) / length, 0.0, 1.0));
	}
	const net::WeaponTimersWire *a = active_timers(p_prev);
	const bool same = a != nullptr && p_prev.active == p_curr.active && p_prev.slots[p_prev.active < sw::kMaxWeaponSlots ? p_prev.active : 0].def == slot.def;
	const float t = float(f);
	if (same) {
		p.spread_deg = a->spread + (b->spread - a->spread) * t;
		p.recoil.pitch = a->recoil.pitch + (b->recoil.pitch - a->recoil.pitch) * t;
		p.recoil.yaw = a->recoil.yaw + (b->recoil.yaw - a->recoil.yaw) * t;
	} else {
		p.spread_deg = b->spread;
		p.recoil = b->recoil;
	}
	p.zoomed = (b->flags & sw::slot_flags::kZoomWasDown) != 0;
	p.events = p_curr.events;
	return p;
}

}
