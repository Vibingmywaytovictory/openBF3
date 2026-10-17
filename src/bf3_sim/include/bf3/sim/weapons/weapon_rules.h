#pragma once
// The M2a integration rules around wp's step_soldier_weapons (docs/design/m2a.md §5.2; owner sv). Both ends call
// only this wrapper, never step_soldier_weapons directly: the GameServer at §4.1 step 4a and the client's
// sim::catch_up (§8.2). wp's own tests (W1-W11) do not call it, so they stay bit-equal by construction, and wp's
// files are not edited for these rules.
//
// The two rules, both of which change predicted weapon state and therefore run in the same bf3_sim code on both ends:
//   * Zoom refused while deploying or sprinting [inferred BF3 behaviour]: before stepping, the wrapper clears kZoom
//     from its copy of the command and sets ctx.sway.zoom = Zoom::NoZoom when the active slot's phase is Deploying
//     with phase_end after the tick's start, or the command switches to another valid slot (a deploy starts this
//     tick), or ctx.sprinting. The whole tick is refused (a deploy that ends mid-tick zooms from the next tick,
//     §16 Q21); spread, recoil and the slot's kZoomWasDown flag then see no zoom.
//   * The fire-mode counter at spawn: spawn_soldier_weapons zeroes every slot's mode_counter, so the first command
//     after a spawn would cycle the mode by the client's current counter. When p_command.tick == p_spawn_tick + 1
//     the wrapper first writes the command's counter into every slot's mode_counter, so that command makes 0 steps.
// p_spawn_tick is the life's spawn tick, which both ends know (the server's life stamp; the client's Spawned
// event, kept in the prediction ring entry).
//
// W2.0 (the wave-2 contract commit) declares this and links a forwarding stub (weapon_rules.cpp) that applies
// neither rule yet; sv replaces the stub body, and m2a.contract's three step_weapons_m2a rows turn on with it.

#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>

namespace bf3::sim::weapons {

void step_weapons_m2a(const WeaponTable &p_table, SoldierWeaponsState &r_state, const Command &p_command, WeaponContext p_context,
		uint64_t p_match_seed, Tick p_spawn_tick, WeaponTickOutput &r_out);

} // namespace bf3::sim::weapons
