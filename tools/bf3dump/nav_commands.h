#pragma once
// bf3dump nav and bots (spike S1 server scale; docs/design/s1.md §1, §5, §6, §12): the navmesh baked from the motor's
// collision set, its components and the G2 rows, Detour query costs, the soup objects near a point, and the 64-walker
// scale bench. Every command reads the install read-only and prints install.unchanged.
//
//   nav bake <LEVEL> <MODE> [--threads=N] [--layers=movement|all] [--radius=ceil|nearest] [--cs=M] [--no-links] [--tiles]
//       G4: the uncached bake as sim.nav.bake times it (run_nav_bake_bench); --tiles adds one row per tile; --cs a
//       cell size that divides 64 m (the §5.7 experiments; default 0.2, radius ceil: 2 cells = 0.4 m).
//   nav stats <LEVEL> <MODE> [...]      the same with the per-tile rows (memory per tile).
//   nav components <LEVEL> <MODE> [--points] [--seams] [--layers=movement|all] [--radius=ceil|nearest] [--cs=M]
//       [--threads=N] [--cache=<dir>] [--dump=<file>]
//       the level x mode as the bots load it (combat tiles over every MP mode, the S1 static world with the terrain
//       cover, the nav): the components; --points the G2 rows (flags, HQs, MCOMs, spawns: offsets, SCC, weak
//       component, class: main / exit-only / entry-only / disconnected / off-mesh); --seams the places where the
//       largest component meets the next ones (docs §5.7); --dump the debug mesh as text ("T" triangles with their
//       component and area, "L" drop links) for an offline picture (derived data: scratch only).
//   nav path <LEVEL> <MODE> [--pairs=N] [--seed=N] [--threads=N] [--cache=<dir>]
//       N seeded full A* queries between random points of the main component: mean / p95 / max ms, lengths.
//   nav probe <LEVEL> <MODE> <x> <y> <z> <radius> [--dump=<file>]
//       the soup placements with triangles within the radius (kind, entity type, res, body, surface, flags, count);
//       --dump writes those triangles as text ("S" rows; scratch only).
//   bots <LEVEL> <MODE> [--walkers=N] [--observers=N] [--seconds=S] [--warmup=S] [--paced=S] [--seed=N] [--threads=N]
//       [--tuning=<motor_tuning.cfg>] [--cache=<dir>]
//       G1 / G3: run_scale_bench, printed as host.scale prints it.

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

const char *nav_usage();
int cmd_nav(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);
int cmd_bots(const bf3::core::InstallReader &p_reader, const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

} // namespace bf3dump
