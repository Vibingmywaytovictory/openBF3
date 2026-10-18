#pragma once
// bf3dump firefight (M2a bt; docs/design/m2a.md §10.2, E13, E15; docs/design/m2a-bt.md): bt's firefight bots on the
// real GameServer, headless (host::run_firefight, the run host.gunplay.firefight / replay / perf make). Reads the install
// read-only and prints install.unchanged.
//
//   firefight <LEVEL> <MODE> [--mode=pairs|free] [--count=N] [--seconds=S] [--warmup=S] [--seed=N] [--observers=N]
//       [--slots=N] [--no-walk] [--no-checks] [--bots] [--perf] [--threads=N] [--tuning=<motor_tuning.cfg>] [--mechanics=<cfg>]
//       [--cache=<dir>]
//       --slots defaults to max(64, bots + observers) (slots 64+ are never hit: lag_comp.cpp kMaxLagSlots).
//       --perf is E13's row (Pairs, 64 bots, 1 observer, 10 s warm-up, 120 s; the p99 against 8.0 ms); --bots adds one
//       row per bot (team, kit, shots, hits, kills, deaths, lives, metres, the smallest 20 s displacement).

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

const char *firefight_usage();
int cmd_firefight(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

} // namespace bf3dump
