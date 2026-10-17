#pragma once
// The oracle rows of docs/design/m2a-weapons.md §1 that a table of gate weapons answers (W1 fire timing, W2 drop
// and range, W3 shots to kill, W5 zone damage, W6 reload timing, W9 explosives), as one function, so the install
// test (sim.weapons, defs resolved from MP_Subway), the unit test (sim.weapons.unit, synthetic defs carrying the
// same numbers) and `bf3dump weapon-sim --check` run the same checks against the same thresholds.
//
// Every row runs on simulated time (integers) through the same code the server will run: run_fire_timing for the
// weapon state machine, ProjectileWorld over a synthetic wall world (range_wall_triangles) for the flights,
// damage.h for the arithmetic. So the rows are exact and do not depend on the machine's load.

#include "bf3/sim/weapons/shooting_range.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::sim::weapons {

struct WeaponCheckRow {
	std::string gate; // "W1" ...
	std::string name;
	bool pass = false;
	std::string measured;
	std::string threshold;
	bool unmeasured = false; // depends on a Mechanics choice S5 has not measured (Mechanics::measured false)
	bool reported = false; // printed for the record, not gated (pass is true)
};

struct WeaponCheckInputs {
	std::shared_ptr<const WeaponTable> table; // the gate variants with their real magazines
	std::shared_ptr<const WeaponTable> oracle; // the same defs built with infinite_magazine (W1's 10 s row)
	float health = 100.0f; // SoldierDef MaxHealth
	float eye_height = 1.6f; // stand eye (SoldierDef)
	uint64_t match_seed = 1;
};

// The variants the rows look up by WeaponSimDef::variant (a missing one fails its rows).
std::span<const std::string_view> weapon_check_variants();

std::vector<WeaponCheckRow> run_weapon_checks(const WeaponCheckInputs &p_inputs);

// "PASS gate name: measured (threshold)" lines and the summary "WEAPON-SIM CHECK: PASS (n/n)".
std::string format_weapon_checks(std::span<const WeaponCheckRow> p_rows);

} // namespace bf3::sim::weapons
