#include "bf3/host/presentation_weapon.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace sw = sim::weapons;

namespace {

double seconds_at(int32_t p_offset, sim::Tick p_tick) {
	if (p_offset == net::kWeaponsWireTimeZero) {
		return 0.0;
	}
	return sw::units_to_seconds(sw::tick_start(p_tick) + sw::SimTime(p_offset));
}

}

const net::WeaponTimersWire *active_weapon_timers(const net::WeaponsWire &p_wire) {
	return p_wire.timers[0].slot == p_wire.active ? &p_wire.timers[0] : nullptr;
}

float held_dispersion(float p_prev_deg, float p_curr_deg, double p_fraction) {
	return p_fraction < 1.0 ? p_prev_deg : p_curr_deg;
}

PresentedWeapon present_weapon(const net::WeaponsWire &p_prev, const net::WeaponsWire &p_curr, sim::Tick p_curr_tick,
		double p_fraction, const sw::WeaponTable &p_table, const sw::SwayContext &p_sway,
		std::span<const uint8_t> p_variants) {
	PresentedWeapon p;
	if (p_curr.active >= sw::kMaxWeaponSlots) {
		return p;
	}
	const net::WeaponSlotWire &slot = p_curr.slots[p_curr.active];
	const net::WeaponTimersWire *b = active_weapon_timers(p_curr);
	if (slot.def == sw::kNoWeapon || slot.def >= p_table.defs.size() || b == nullptr) {
		return p;
	}
	const uint8_t variant = p_curr.active < p_variants.size() ? p_variants[p_curr.active] : uint8_t(0);
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
	p.magazine_capacity = p_table.def(slot.def, variant).magazine_capacity;
	const sim::Tick prev_tick = p_curr_tick > 0 ? p_curr_tick - 1u : 0u;
	p.time_s = sw::units_to_seconds(sw::tick_end(prev_tick)) + f * sw::units_to_seconds(sw::kTimeUnitsPerTick);
	p.phase_start_s = seconds_at(b->phase_start, p_curr_tick);
	p.phase_end_s = seconds_at(b->phase_end, p_curr_tick);
	const double length = p.phase_end_s - p.phase_start_s;
	if (b->phase_end != net::kWeaponsWireTimeZero && length > 0.0) {
		p.phase_progress = float(std::clamp((p.time_s - p.phase_start_s) / length, 0.0, 1.0));
	}
	const net::WeaponTimersWire *a = active_weapon_timers(p_prev);
	const bool same = a != nullptr && p_prev.active == p_curr.active && p_prev.slots[p_prev.active < sw::kMaxWeaponSlots ? p_prev.active : 0].def == slot.def;
	const float t = float(f);
	const sw::WeaponSimDef &def = p_table.def(slot.def, variant);
	const sw::RecoilOffset curr = sw::camera_recoil(def, b->sway, p_sway.zoom);
	if (same) {
		const sw::RecoilOffset prev = sw::camera_recoil(def, a->sway, p_sway.zoom);
		p.recoil.pitch = prev.pitch + (curr.pitch - prev.pitch) * t;
		p.recoil.yaw = prev.yaw + (curr.yaw - prev.yaw) * t;
		p.spread_deg = held_dispersion(a->sway.dispersion, b->sway.dispersion, f);
	} else {
		p.recoil = curr;
		p.spread_deg = b->sway.dispersion;
	}
	p.zoomed = (b->flags & sw::slot_flags::kZoomWasDown) != 0;
	p.events = p_curr.events;
	return p;
}
}
