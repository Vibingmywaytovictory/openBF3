#pragma once
// weapon_mechanics.cfg: the unmeasured weapon mechanics (mechanics.h) as a tuning file (docs/design/m2a.md §5.5;
// owner sv). Parsed on both ends at level load: the server from GunplayConfig::mechanics_cfg, the client from
// ClientSessionConfig::mechanics_cfg, each when it builds its WeaponTable. Mechanics is part of weapon_table_hash,
// so a client whose file differs is refused on MatchInfo ("weapon table mismatch", E6). When S5 Session 1 decides,
// the change is one line per switch plus `measured = true`; no code changes.
//
// Format: motor_tuning.cfg's (soldier_motor.h): "key = value" lines, '#' comments, blank lines; keys are the
// Mechanics field names, enums by name in snake_case (uniform_area | uniform_radius, linear | exponential,
// on_trigger_release | when_recovered, immediate | at_decrease_rate, straight_line | path_length), booleans
// true / false, floats finite. Unknown keys, a missing '=', a duplicate key and bad values are errors naming the
// line. Keys left out keep the defaults (wp's, mechanics.h). Hostile input: a text above kMaxMechanicsTuningBytes or
// with more than kMaxMechanicsTuningLines lines is refused before any line is parsed.

#include "bf3/core/result.h"
#include "bf3/sim/weapons/mechanics.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace bf3::sim::weapons {

inline constexpr size_t kMaxMechanicsTuningBytes = 64 * 1024;
inline constexpr size_t kMaxMechanicsTuningLines = 1024;
// The shipped file, relative to the Godot project directory (bf3-opensource/); the headless tests pass its absolute path.
inline constexpr std::string_view kMechanicsTuningPath = "config/weapon_mechanics.cfg";

core::Result<Mechanics> parse_mechanics_tuning(std::string_view p_text);
std::string format_mechanics_tuning(const Mechanics &p_mechanics); // every key; parse_mechanics_tuning reads it back

} // namespace bf3::sim::weapons
