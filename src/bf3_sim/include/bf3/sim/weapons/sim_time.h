#pragma once
// Sub-tick time for the weapon simulation (docs/design/m2a-weapons.md §5.1; plan §3.2 "sub-tick shots", D7).
//
// A shot between two ticks keeps its exact time, so a rate of fire is exact at any tick rate: 800 RPM is a
// 75 ms interval over any length of fire, not "4 or 5 ticks". Times are integers in units of 1 / (kSimHz x 10^6)
// s, so a tick is exactly kTimeUnitsPerTick units (a microsecond-based time is not: 1/60 s is 16,666.67 us), and
// every interval of the data that is a multiple of 1/(60 x 10^6) s is exact: 75 ms = 4,500,000, 50 ms (1200 RPM)
// = 3,000,000, 1.3 s (the M98B bolt) = 78,000,000, 0.48 s (the 870 pump) = 28,800,000, 2.37 s = 142,200,000.
// Other intervals (650 RPM = 92.307... ms) are rounded once, to the nearest unit (16.7 ns), when the def is made
// (make_weapon_sim_def); the rounded value is part of the def and its hash, so the server and every client add
// the same integer forever and never drift from each other.
//
// Tick convention (the M1 one, soldier_state.h): the command for tick T drives the step that ends at time
// tick_end(T) = T x U; that step covers [tick_start(T), tick_end(T)). A trigger held in command T fires its first
// shot at tick_start(T) (or later, when the weapon is not ready yet). int64 units last 4.9 million years at 60 Hz.

#include "bf3/sim/sim_types.h"

#include <cstdint>

namespace bf3::sim::weapons {

using SimTime = int64_t;

inline constexpr int64_t kTimeUnitsPerTick = 1'000'000;
inline constexpr int64_t kTimeUnitsPerSecond = kTimeUnitsPerTick * int64_t(kSimHz);
// The largest time a def may hold (60 s): every data time is checked against it (hostile-input bound, §9).
inline constexpr SimTime kMaxDefTime = 60 * kTimeUnitsPerSecond;

constexpr SimTime tick_start(Tick p_tick) {
	return (int64_t(p_tick) - 1) * kTimeUnitsPerTick;
}
constexpr SimTime tick_end(Tick p_tick) {
	return int64_t(p_tick) * kTimeUnitsPerTick;
}
// The tick whose step covers p_time (tick_start(T) <= p_time < tick_end(T)); p_time >= 0.
constexpr Tick tick_containing(SimTime p_time) {
	return Tick(p_time / kTimeUnitsPerTick + 1);
}
// Where in its tick p_time lies, 0 (the tick's start) .. 1 (exclusive).
constexpr double tick_fraction(SimTime p_time) {
	return double(p_time % kTimeUnitsPerTick) / double(kTimeUnitsPerTick);
}
constexpr double units_to_seconds(SimTime p_units) {
	return double(p_units) / double(kTimeUnitsPerSecond);
}

// Seconds of data -> units, rounded to the nearest unit. Only for finite, non-negative values up to the def
// bound; make_weapon_sim_def checks that before it calls this (a negative or NaN input gives 0 here).
constexpr SimTime seconds_to_units(double p_seconds) {
	if (!(p_seconds > 0.0)) {
		return 0;
	}
	const double u = p_seconds * double(kTimeUnitsPerSecond) + 0.5;
	return u >= double(kMaxDefTime) ? kMaxDefTime : SimTime(u);
}
// The interval of a rate in rounds per minute: 60 s / rpm, computed as (60 x units per second) / rpm so the
// common rates are exact in double (800 -> 4,500,000.0). p_rpm must be > 0 (checked by the def builder).
constexpr SimTime rpm_to_interval(double p_rpm) {
	if (!(p_rpm > 0.0)) {
		return kMaxDefTime;
	}
	const double u = (60.0 * double(kTimeUnitsPerSecond)) / p_rpm + 0.5;
	return u >= double(kMaxDefTime) ? kMaxDefTime : SimTime(u);
}

} // namespace bf3::sim::weapons
