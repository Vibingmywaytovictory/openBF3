#pragma once

#include "bf3/core/gameplay/specializations.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstddef>
#include <cstdint>

namespace bf3::host {

int32_t spawn_magazines(
		const sim::weapons::WeaponSimDef &p_def, size_t p_slot, const core::gameplay::SpecializationEffects &p_effects);

void apply_spawn_effects(sim::weapons::SoldierWeaponsState &r_state, const sim::weapons::WeaponTable &p_table,
		const core::gameplay::SpecializationEffects &p_effects);

sim::MotorModifiers spawn_motor_modifiers(
		const sim::MotorParams &p_params, const core::gameplay::SpecializationTable &p_table, uint32_t p_mask);

}
