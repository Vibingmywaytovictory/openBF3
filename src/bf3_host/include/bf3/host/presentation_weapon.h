#pragma once
// The own weapon as the presentation sees it (docs/design/m2a.md §8.3, §11; owner cl). Godot-free, plain data. The
// client thread publishes the wire views of the two latest predicted ticks in the PresentationFrame (cl's additive
// block); present_weapon interpolates them per display frame into one PresentedWeapon, which the viewmodel (vm),
// the ADS camera and the HUD feed (hud) read. vm and hud code against this struct from the start of wave 2 with
// scripted values (their demo scenes); cl provides real frames.
//
// Times are in seconds of sim time (SimTime / kTimeUnitsPerSecond, the predicted timeline) so the animator needs no
// bf3_sim types; phases are sim::weapons::WeaponPhase values.

#include "bf3/net/weapons_wire.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>
#include <type_traits>

namespace bf3::host {

struct PresentedWeapon {
	bool valid = false; // a live own soldier with a weapon in hand
	uint16_t def = sim::weapons::kNoWeapon; // the active slot's def id (WeaponTable / HudStatic row)
	uint8_t active_slot = 0;
	uint8_t pending_slot = 0xFF; // switching to (0xFF = none)
	sim::weapons::WeaponPhase phase = sim::weapons::WeaponPhase::None;
	uint8_t mode = 0; // index into WeaponSimDef::modes
	uint8_t flags = 0; // sim::weapons::slot_flags:: of the active slot
	int32_t magazine = 0;
	int32_t reserve = 0;
	int32_t magazine_capacity = 0;
	double time_s = 0; // the presented sim time (prev tick end + fraction)
	double phase_start_s = 0; // the active phase's start and end (end 0 = untimed)
	double phase_end_s = 0;
	float phase_progress = 0; // 0..1 of the timed phase at time_s (reload percent, deploy)
	float spread_deg = 0; // interpolated predicted spread (the crosshair, HudWeapon::dispersion)
	sim::weapons::RecoilOffset recoil{}; // interpolated predicted recoil offset (the camera adds it, E19)
	bool zoomed = false; // the predicted kZoomWasDown of the active slot (step_weapons_m2a clears it while deploying / sprinting)
	uint32_t events = 0; // weapon_events:: of the newer tick (cues come from PresentEvents, not from here)
};
static_assert(std::is_trivially_copyable_v<PresentedWeapon>);

// p_prev / p_curr: the wire views of the predicted ticks p_curr_tick - 1 and p_curr_tick; p_fraction 0..1 between
// them (present_own_fraction). Continuous fields lerp (spread, recoil, time); discrete fields come from p_curr.
PresentedWeapon present_weapon(const net::WeaponsWire &p_prev, const net::WeaponsWire &p_curr, sim::Tick p_curr_tick, double p_fraction,
		const sim::weapons::WeaponTable &p_table);

} // namespace bf3::host
