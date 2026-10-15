#pragma once
// bf3dump spawns command (M1 T2; docs/design/m1.md §10.1): the soldier spawn points and the character spawners of
// one level x mode (bf3_core level/spawn_points.h), as TSV on stdout.

#include "bf3/core/install.h"

#include <memory>
#include <string>

namespace bf3dump {

// `spawns <LEVEL> <MODE>`: one `point` row per AlternateSpawn placement (index, object, position, yaw, team,
// enabled, priority, partition) and one `spawner` row per CharacterSpawnReference; counts and timings on stderr.
int cmd_spawns(const std::shared_ptr<bf3::core::Install> &p_install, const std::string &p_level, const std::string &p_mode);

} // namespace bf3dump
