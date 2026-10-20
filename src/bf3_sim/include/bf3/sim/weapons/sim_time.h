#pragma once

#include "bf3/sim/sim_types.h"

#include <cstdint>

namespace bf3::sim::weapons {

using SimTime = int64_t;

inline constexpr int64_t kTimeUnitsPerTick = 1'000'000;
inline constexpr int64_t kTimeUnitsPerSecond = kTimeUnitsPerTick * int64_t(kSimHz);

inline constexpr SimTime kMaxDefTime = 60 * kTimeUnitsPerSecond;

constexpr SimTime tick_start(Tick p_tick) {
	return (int64_t(p_tick) - 1) * kTimeUnitsPerTick;
}
constexpr SimTime tick_end(Tick p_tick) {
	return int64_t(p_tick) * kTimeUnitsPerTick;
}

constexpr Tick tick_containing(SimTime p_time) {
	return Tick(p_time / kTimeUnitsPerTick + 1);
}

constexpr double tick_fraction(SimTime p_time) {
	return double(p_time % kTimeUnitsPerTick) / double(kTimeUnitsPerTick);
}
constexpr double units_to_seconds(SimTime p_units) {
	return double(p_units) / double(kTimeUnitsPerSecond);
}

constexpr SimTime seconds_to_units(double p_seconds) {
	if (!(p_seconds > 0.0)) {
		return 0;
	}
	const double u = p_seconds * double(kTimeUnitsPerSecond) + 0.5;
	return u >= double(kMaxDefTime) ? kMaxDefTime : SimTime(u);
}

constexpr SimTime scale_units(SimTime p_units, double p_fraction) {
	if (!(p_fraction > 0.0) || p_units <= 0) {
		return 0;
	}
	if (p_fraction >= 1.0) {
		return p_units;
	}
	return SimTime(double(p_units) * p_fraction + 0.5);
}

constexpr SimTime rpm_to_interval(double p_rpm) {
	if (!(p_rpm > 0.0)) {
		return kMaxDefTime;
	}
	const double u = (60.0 * double(kTimeUnitsPerSecond)) / p_rpm + 0.5;
	return u >= double(kMaxDefTime) ? kMaxDefTime : SimTime(u);
}

}
