#pragma once

#include "bf3/net/weapons_wire.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>
#include <type_traits>

namespace bf3::host {

struct PresentedWeapon {
	bool valid = false;
	uint16_t def = sim::weapons::kNoWeapon;
	uint8_t active_slot = 0;
	uint8_t pending_slot = 0xFF;
	sim::weapons::WeaponPhase phase = sim::weapons::WeaponPhase::None;
	uint8_t mode = 0;
	uint8_t flags = 0;
	int32_t magazine = 0;
	int32_t reserve = 0;
	int32_t magazine_capacity = 0;
	double time_s = 0;
	double phase_start_s = 0;
	double phase_end_s = 0;
	float phase_progress = 0;
	float spread_deg = 0;
	sim::weapons::RecoilOffset recoil{};
	bool zoomed = false;
	uint32_t events = 0;
};
static_assert(std::is_trivially_copyable_v<PresentedWeapon>);

PresentedWeapon present_weapon(const net::WeaponsWire &p_prev, const net::WeaponsWire &p_curr, sim::Tick p_curr_tick, double p_fraction,
		const sim::weapons::WeaponTable &p_table, const sim::weapons::SwayContext &p_sway);

}
