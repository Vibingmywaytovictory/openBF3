#pragma once
// bf3dump S2 vehicle commands (docs/design/s2.md §1, §7.3, §8): the Humvee's drive def and model, and the S2 replay
// harness with the same gate table the sim.vehicle tests print.

#include "bf3/core/install.h"

#include <memory>
#include <string>
#include <vector>

namespace bf3dump {

// `vehicledrive <LEVEL> <MODE> [<blueprint>] [--tuning=<cfg>]`: the VehicleDriveDef as read (bf3_core
// gameplay/vehicle_drive.h) and the VehicleModel built from it (bf3_sim vehicle_model.h). Default blueprint
// vehicles/humveearmored/humveearmored.
int cmd_vehicledrive(const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

// `vehicle-replay <LEVEL> <MODE> [options]`: the course search on the level's static world, then gates D, G1, G1r and
// G2 per found course, as the sim.vehicle test runs them. Options:
//   --synthetic          the 7 synthetic courses on build_synthetic_vehicle_ground (the model still from LEVEL x MODE)
//   --courses=a,b,...    only these kinds (straight, brake, slalom, circle, bumps, collision, slope_idle)
//   --modes=<hex>        VehicleReplayOptions::modes (default 3f: all six restore modes)
//   --ticks=N            run length (default 3600)
//   --no-g1 / --no-g2    skip G1 + G1r / G2
//   --handling           the handling numbers (report H) instead of the courses
//   --sweep              G2 OwnHistory per ghost quantisation row (s2.md §7.3), max / p99 per course (no G1)
//   --hash-out=<file>    the server's state_hash() per tick, one hex line per tick, per course (gate X)
//   --log-out=<dir>      each course's command log, <dir>/<kind>.vlog (scratch only, never committed)
//   --log-in=<file>      replay a saved log open loop on the course of --courses (exactly one kind) and print the
//                        final hashes; with --hash-out the per-tick hashes
//   --tuning=<cfg>       a vehicle_tuning.cfg instead of the built-in defaults (= the shipped file)
//   --search-radius=m, --turnaround=m, --flat=deg   VehicleCourseSearch radius, turnaround_radius, max_flat_degrees
//   --trace=N            the server run of each selected course, one row every N ticks (and every chassis contact)
int cmd_vehicle_replay(const std::shared_ptr<bf3::core::Install> &p_install, const std::vector<std::string> &p_args);

const char *vehicle_usage();

} // namespace bf3dump
