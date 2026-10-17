// step_weapons_m2a: the two M2a integration rules around wp's step_soldier_weapons (docs/design/m2a.md §5.2,
// §16 Q21; owner sv; weapon_rules.h). Both ends call only this: the GameServer (gunplay.cpp, §4.1 step 4a) and the
// client's sim::catch_up (cl, §8.2). wp's files are not touched by these rules, so W1-W11 stay bit-equal.
#include "bf3/sim/weapons/weapon_rules.h"

#include "bf3/sim/weapons/weapon_input.h"

namespace bf3::sim::weapons {

namespace {

bool valid_slot(const WeaponTable &p_table, const WeaponSlotState &p_slot) {
	return p_slot.def != kNoWeapon && p_slot.def < p_table.defs.size();
}

} // namespace

void step_weapons_m2a(const WeaponTable &p_table, SoldierWeaponsState &r_state, const Command &p_command, WeaponContext p_context,
		uint64_t p_match_seed, Tick p_spawn_tick, WeaponTickOutput &r_out) {
	Command command = p_command;
	// Both rules read the state the step will start from, so they act only when the step will run (its precondition:
	// the command is for the tick after the state's). Otherwise step_soldier_weapons changes nothing and so do we.
	if (p_command.tick == r_state.tick + 1 && r_state.active < kMaxWeaponSlots) {
		const WeaponInput in = weapon_input(p_command);
		// The fire-mode counter at spawn: spawn_soldier_weapons zeroed every slot's mode_counter, so the first command
		// of the life would cycle the mode by the client's current counter. That command adopts the counter instead
		// (0 steps); a later change steps as usual.
		if (p_command.tick == p_spawn_tick + 1) {
			for (WeaponSlotState &s : r_state.slots) {
				s.mode_counter = in.fire_mode_counter;
			}
		}
		// Zoom refused for the whole tick while deploying (a deploy whose end is after the tick's start, including
		// the one that ends inside this tick), when the command starts a switch to another filled slot (its deploy
		// starts this tick), or while sprinting.
		const WeaponSlotState &active = r_state.slots[r_state.active];
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

} // namespace bf3::sim::weapons
