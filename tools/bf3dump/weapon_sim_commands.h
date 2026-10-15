#pragma once
// bf3dump M2a weapons commands (docs/design/m2a-weapons.md §10): weapon-sim and range, the weapon simulation of
// bf3_sim (weapons/) run on defs resolved from the install (read-only, weapon_install.h). Output is BF3 data and stays
// out of the repo. Each command ends with the install.unchanged row and exits 1 when the install changed.

#include "ebx_commands.h"

#include <string>
#include <vector>

namespace bf3dump {

// weapon-sim <partition> [--unlock <u>]... [--level <sb>] [--seed N] [--seconds S]: the def's sim numbers, fire timing
//   (the first 20 shot times, gaps, shots in S s), reload timing, the drop table, shots to kill and one-shot ranges.
// weapon-sim --all [--level <sb>] [--threads N]: W4's TSV, one row per qualifying infantry variant (exit 1 on an error).
// weapon-sim --check [--level <sb>]: the W1-W6 / W9 data rows as PASS / FAIL lines, "WEAPON-SIM CHECK: PASS (n/n)",
//   exit 0 / 1 (ctest tools.weapon_sim).
int cmd_weapon_sim(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

// range <partition> [--level MP_Subway] [--mode TeamDeathMatch0] [--distances 10,25,50] [--zone head|chest|body|legs]
//   [--script hold|tap|throw] [--seconds S] [--seed N] [--exact] [--perf]: the shooting range on the level's static
//   world (lane, per target hits per zone, damage, kill shot and time, list hashes, impact materials, diagnostics);
//   --perf runs W11's 64-shooter load (run_range_perf) instead.
int cmd_range(const EbxContext &p_ctx, const std::vector<std::string> &p_args);

const char *weapon_sim_usage();

} // namespace bf3dump
