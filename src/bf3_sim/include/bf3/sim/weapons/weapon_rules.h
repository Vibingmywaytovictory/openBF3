#pragma once

#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>

namespace bf3::sim::weapons {

void step_weapons_with_rules(const WeaponTable &p_table, SoldierWeaponsState &r_state, const Command &p_command, WeaponContext p_context,
		uint64_t p_match_seed, Tick p_spawn_tick, WeaponTickOutput &r_out);

}
