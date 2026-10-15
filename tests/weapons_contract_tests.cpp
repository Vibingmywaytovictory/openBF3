// M2a weapons interface contract (docs/design/m2a-weapons.md §14): every weapons header compiles together with
// the M1 headers it builds on, the plain types keep their properties, and the inline helpers the server and the
// clients must agree on (sub-tick time, the counter-based RNG, the weapon button fields) behave. No data; the
// functions the headers only declare are the implementation wave's and are not called here.

#include "bf3/core/gameplay/grid_pairs.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/mechanics.h"
#include "bf3/sim/weapons/shooting_range.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim/weapons/surface_materials.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <doctest/doctest.h>

#include <type_traits>

using namespace bf3::sim;
using namespace bf3::sim::weapons;

// ---- sub-tick time (§5.1) --------------------------------------------------------------------------------------
static_assert(kTimeUnitsPerSecond == 60'000'000, "the contract's numbers assume the 60 Hz build");
static_assert(tick_start(1) == 0 && tick_end(1) == kTimeUnitsPerTick);
static_assert(tick_containing(0) == 1 && tick_containing(kTimeUnitsPerTick - 1) == 1 && tick_containing(kTimeUnitsPerTick) == 2);
// The §7.1 intervals are exact in units.
static_assert(rpm_to_interval(800.0) == 4'500'000); // M16A3 75 ms
static_assert(rpm_to_interval(1200.0) == 3'000'000); // AN-94 burst 50 ms
static_assert(rpm_to_interval(600.0) == 6'000'000); // AN-94 between bursts 100 ms
static_assert(seconds_to_units(1.3) == 78'000'000); // M98B bolt = 78 ticks
static_assert(seconds_to_units(0.48) == 28'800'000); // 870 pump
static_assert(seconds_to_units(1.8) == 108'000'000); // M16A3 tactical reload
static_assert(seconds_to_units(2.37) == 142'200'000); // M16A3 empty reload
static_assert(seconds_to_units(0.67) == 40'200'000); // M16A3 deploy, 870 shell
static_assert(seconds_to_units(3.6) == 216'000'000); // M67 fuse
static_assert(rpm_to_interval(650.0) == 5'538'462); // rounded once to the nearest unit (16.7 ns)
static_assert(seconds_to_units(-1.0) == 0 && seconds_to_units(1e9) == kMaxDefTime && rpm_to_interval(0.0) == kMaxDefTime);

// ---- the plain types the rings, the wire and the events copy -------------------------------------------------
static_assert(std::is_trivially_copyable_v<WeaponSlotState>);
static_assert(std::is_trivially_copyable_v<SoldierWeaponsState>);
static_assert(std::is_trivially_copyable_v<ShotEvent>);
static_assert(std::is_trivially_copyable_v<Projectile>);
static_assert(std::is_trivially_copyable_v<ImpactEvent>);
static_assert(std::is_trivially_copyable_v<DamageEvent>);
static_assert(std::is_trivially_copyable_v<HitTarget>);
static_assert(std::is_trivially_copyable_v<WeaponContext>);
static_assert(kHitZoneCount == 4 && size_t(HitZone::Legs) == 3);
static_assert(uint8_t(RngPurpose::SpreadRadius) == 0 && uint8_t(RngPurpose::FuseVariation) == 5, "RNG purposes are append-only");

TEST_SUITE("weapons-contract") {
	TEST_CASE("weapons contract: 800 RPM over 10 s of fire is 134 shots exactly 75 ms apart") {
		const SimTime interval = rpm_to_interval(800.0);
		const SimTime start = tick_start(61); // fire pressed in the command of tick 61
		int shots = 0;
		SimTime last = -1;
		for (SimTime t = start; t < start + 10 * kTimeUnitsPerSecond; t += interval) {
			if (last >= 0) {
				CHECK(t - last == 4'500'000);
			}
			last = t;
			++shots;
		}
		CHECK(shots == 134);
		// The 134th shot lies 9.975 s (598.5 ticks) after the first, half way through its tick.
		CHECK(last - start == 133 * 4'500'000);
		CHECK(tick_containing(last) == 61 + 598);
		CHECK(tick_fraction(last) == doctest::Approx(0.5));
	}

	TEST_CASE("weapons contract: SplitMix64 and the shot seeds are pinned") {
		// SplitMix64's first output for state 0 (the reference implementation's published value).
		static_assert(splitmix64(0) == 0xE220A8397B1DCDAFull);
		// Changing the seed derivation changes every shot of every match: these values are the contract.
		static_assert(shot_seed(1, 0, 0) == 0x5E41AB087439611Eull);
		static_assert(shot_seed(1, 3, 7) == 0x61B96DC53F613DD5ull);
		static_assert(shot_seed(0x0123456789ABCDEFull, 5, 1000) == 0xA5ABD4EB4FCCEE50ull);
		static_assert(shot_random(shot_seed(1, 3, 7), RngPurpose::SpreadAngle, 0) == 0x1B8967EE9F05450Aull);
		CHECK(shot_seed(1, 3, 7) != shot_seed(1, 3, 8));
		CHECK(shot_seed(1, 3, 7) != shot_seed(1, 4, 7));
		CHECK(shot_seed(1, 3, 7) != shot_seed(2, 3, 7));
		CHECK(shot_random(42, RngPurpose::PelletRadius, 0) != shot_random(42, RngPurpose::PelletRadius, 1));
		CHECK(unit_float(0) == 0.0f);
		CHECK(unit_float(~0ull) < 1.0f);
	}

	TEST_CASE("weapons contract: weapon buttons are levels that survive the server's repeat") {
		WeaponInput in;
		in.fire = true;
		in.zoom = true;
		in.fire_mode_counter = 2;
		in.select_slot = 1;
		Command c;
		c.tick = 10;
		c.buttons = with_weapon_input(buttons::kJump | buttons::kSprint, in);
		CHECK(weapon_input(c) == in);
		CHECK((c.buttons & buttons::kJump) != 0);
		const Command r = repeated_command(c, 11);
		CHECK(weapon_input(r) == in); // a level: the repeat neither drops nor re-triggers it
		CHECK((r.buttons & buttons::kJump) == 0);
		WeaponInput keep;
		c.buttons = with_weapon_input(0, keep);
		CHECK(weapon_input(c).select_slot == 0xFF);
		c.buttons = weapon_buttons::kSlotMask; // 15: out of range -> keep
		CHECK(weapon_input(c).select_slot == 0xFF);
	}

	TEST_CASE("weapons contract: the unmeasured mechanics default to the plan's no-measurement choice") {
		const Mechanics m;
		CHECK(m.disc == DiscDistribution::UniformArea); // plan §3.13 "Without VU"
		CHECK(m.recovery == RecoveryShape::Linear);
		CHECK_FALSE(m.measured);
		CHECK(m.suppression_threshold == doctest::Approx(0.3f));
		CHECK(m.bullet_damage_modifier == 1.0f);
	}
}
