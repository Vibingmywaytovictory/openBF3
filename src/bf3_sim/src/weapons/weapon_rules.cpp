#include "bf3/sim/weapons/weapon_rules.h"

#include "bf3/sim/weapons/weapon_input.h"

namespace bf3::sim::weapons {

namespace {

bool valid_slot(const WeaponTable &p_table, const WeaponSlotState &p_slot) {
	return p_slot.def != kNoWeapon && p_slot.def < p_table.defs.size();
}

}

void step_weapons_with_rules(const WeaponTable &p_table, SoldierWeaponsState &r_state, const Command &p_command, WeaponContext p_context,
		uint64_t p_match_seed, Tick p_spawn_tick, WeaponTickOutput &r_out) {
	Command command = p_command;

	if (p_command.tick == r_state.tick + 1 && r_state.active < kMaxWeaponSlots) {
		const WeaponInput in = weapon_input(p_command);

		if (p_command.tick == p_spawn_tick + 1) {
			for (WeaponSlotState &s : r_state.slots) {
				s.mode_counter = in.fire_mode_counter;
			}
		}

		const WeaponSlotState &active = r_state.slots[r_state.active];
		// SOURCE: notes R2-A3 checker on B (Zoom forced off while sprinting, like Fire).
		// STAND-IN: zoom is not refused in the RecoverTime window after a sprint, as that checker has it. Settle: R2-A3 B.
		// INFERRED: zoom is also refused for the whole deploy and a pending switch. Settle: a note on zoom during a deploy.
		bool refuse = p_context.sprinting;
		if (active.phase == WeaponPhase::Deploying && active.phase_end > tick_start(p_command.tick)) {
			refuse = true;
		}
		if (in.select_slot < kMaxWeaponSlots && in.select_slot != r_state.active && valid_slot(p_table, r_state.slots[in.select_slot])) {
			refuse = true;
		}
		if (refuse) {
			command.buttons &= ~weapon_buttons::kZoom;
			p_context.sway.zoom = Zoom::NoZoom;
		}
	}
	step_soldier_weapons(p_table, r_state, command, p_context, p_match_seed, r_out);
}

}
