// step_weapons_m2a (docs/design/m2a.md §5.2; owner sv). W2.0 STUB: forwards to step_soldier_weapons without the two
// integration rules, only so the tree links against the contract (weapon_rules.h). sv replaces this body; until it
// does, m2a.contract's step_weapons_m2a rows are reported as pending and nothing else calls the wrapper.
#include "bf3/sim/weapons/weapon_rules.h"

namespace bf3::sim::weapons {

void step_weapons_m2a(const WeaponTable &p_table, SoldierWeaponsState &r_state, const Command &p_command, WeaponContext p_context,
		uint64_t p_match_seed, Tick p_spawn_tick, WeaponTickOutput &r_out) {
	(void)p_spawn_tick;
	step_soldier_weapons(p_table, r_state, p_command, p_context, p_match_seed, r_out);
}

} // namespace bf3::sim::weapons
