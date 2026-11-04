#include "bf3/sim/weapons/suppression.h"

#include "bf3/core/math.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::sim::weapons {

namespace {

std::optional<double> field(const core::gameplay::SoldierDef &p_soldier, std::string_view p_name) {
	for (const auto &[name, value] : p_soldier.suppression) {
		if (name == p_name) {
			return value;
		}
	}
	return std::nullopt;
}

}

core::Result<SuppressionDef> make_suppression_def(const core::gameplay::SoldierDef &p_soldier) {
	SuppressionDef out;
	const std::optional<double> radius = field(p_soldier, "SuppressionSphereRadius");
	if (!radius || field(p_soldier, "Excluded").value_or(0.0) != 0.0) {
		return out;
	}
	struct Bound {
		const char *name;
		double max;
		float *out;
	};
	const Bound bounds[] = { { "SuppressionSphereRadius", 100.0, &out.sphere_radius },
		{ "SuppressionDecay", 100.0, &out.decay }, { "FallOffDelay", 600.0, &out.fall_off_delay },
		{ "FallOffMultiplier", 100.0, &out.fall_off_multiplier },
		{ "SuppressionLowThreshold", 1.0, &out.low_threshold },
		{ "SuppressionHighThreshold", 1.0, &out.high_threshold },
		{ "SuppressionUIThreshold", 1.0, &out.ui_threshold } };
	for (const Bound &b : bounds) {
		const std::optional<double> v = field(p_soldier, b.name);
		if (!v || !std::isfinite(*v) || *v < 0.0 || *v > b.max) {
			return core::make_error("suppression: SoldierSuppressionComponentData." + std::string(b.name) +
					(v ? " = " + std::to_string(*v) + " is out of range" : " is missing"));
		}
		*b.out = float(*v);
	}
	out.aborts_regeneration = field(p_soldier, "SuppressionAbortsHealthRegeneration").value_or(0.0) != 0.0;
	out.check_line_of_sight = field(p_soldier, "CheckLineOfSight").value_or(0.0) != 0.0;
	out.present = out.sphere_radius > 0.0f;
	return out;
}

float suppression_level(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick) {
	const float level = float(p_state.level) / kSuppressionLevelSteps;
	if (p_state.level == 0 || p_tick <= p_state.hit_tick) {
		return level;
	}
	const float seconds = float(p_tick - p_state.hit_tick) / float(kSimHz);
	const float falling = std::max(0.0f, seconds - p_def.fall_off_delay);
	return std::max(0.0f, level - falling * p_def.decay * p_def.fall_off_multiplier);
}

SuppressionState add_suppression(
		const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick, float p_amount) {
	if (!p_def.present || !std::isfinite(p_amount) || !(p_amount > 0.0f)) {
		return p_state;
	}
	const float level = std::clamp(suppression_level(p_def, p_state, p_tick) + p_amount, 0.0f, 1.0f);
	SuppressionState out;
	out.level = uint16_t(std::lround(level * kSuppressionLevelSteps));
	out.hit_tick = p_tick;
	return out;
}

float suppression_multiplier(const ProjectileSimDef &p_projectile, float p_distance) {
	const float lo = p_projectile.suppression_min_distance;
	const float hi = p_projectile.suppression_max_distance;
	if (!(hi > lo) || !(p_distance > lo)) {
		return p_projectile.suppression_min;
	}
	if (p_distance >= hi) {
		return p_projectile.suppression_max;
	}
	const float t = (p_distance - lo) / (hi - lo);
	return p_projectile.suppression_min + (p_projectile.suppression_max - p_projectile.suppression_min) * t;
}

bool passes_near(
		const Vec3 &p_from, const Vec3 &p_to, bool p_first, SegmentEnd p_end, const Vec3 &p_centre, float p_radius) {
	const Vec3 d = core::vec::sub(p_to, p_from);
	const float length2 = core::vec::dot(d, d);
	if (!(length2 > 0.0f)) {
		return false;
	}
	const float along = core::vec::dot(core::vec::sub(p_centre, p_from), d) / length2;
	const bool inside = along >= 0.0f && along < 1.0f;
	const bool before = along < 0.0f && p_first;
	const bool after = along >= 1.0f && p_end == SegmentEnd::Ended;
	if (!(inside || before || after) || !std::isfinite(along)) {
		return false;
	}
	const Vec3 closest = core::vec::add(p_from, core::vec::scale(d, std::clamp(along, 0.0f, 1.0f)));
	const Vec3 off = core::vec::sub(closest, p_centre);
	return core::vec::dot(off, off) <= p_radius * p_radius;
}

}
