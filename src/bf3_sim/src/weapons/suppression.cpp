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

uint16_t suppression_counter(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick) {
	if (p_state.counter == 0 || p_tick <= p_state.hit_tick) {
		return p_state.counter;
	}
	const float seconds = float(p_tick - p_state.hit_tick) / float(kSimHz);
	if (seconds < p_def.fall_off_delay) {
		return p_state.counter;
	}
	const float falling = seconds - p_def.fall_off_delay;
	const float value = float(p_state.counter) -
			falling * falling * kSuppressionFallOffPerSecondSquared * p_def.fall_off_multiplier;
	return uint16_t(std::clamp(value, 0.0f, float(kSuppressionCounterCap)));
}

float suppression_ratio(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick) {
	return float(suppression_counter(p_def, p_state, p_tick)) / float(kSuppressionCounterCap);
}

bool suppressed(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick) {
	return p_def.present && p_state.suppressed && suppression_ratio(p_def, p_state, p_tick) > p_def.low_threshold;
}

float suppression_level(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick) {
	const float r = suppression_ratio(p_def, p_state, p_tick);
	const float low = p_def.low_threshold;
	if (r < low || !(low < 1.0f)) {
		return r - low;
	}
	return (r - low) / (1.0f - low);
}

float suppression_scale(const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick) {
	if (!suppressed(p_def, p_state, p_tick)) {
		return 0.0f;
	}
	const float level = std::max(0.0f, suppression_level(p_def, p_state, p_tick));
	return std::log10(1.0f + kSuppressionLogSpan * level);
}

SuppressionState add_suppression(
		const SuppressionDef &p_def, const SuppressionState &p_state, Tick p_tick, uint32_t p_add) {
	if (!p_def.present || p_add == 0) {
		return p_state;
	}
	const uint32_t now = suppression_counter(p_def, p_state, p_tick);
	SuppressionState out;
	out.counter = uint16_t(std::min<uint32_t>(now + std::min(p_add, kSuppressionCounterCap), kSuppressionCounterCap));
	out.hit_tick = p_tick;
	out.suppressed = suppressed(p_def, p_state, p_tick) ||
			float(out.counter) / float(kSuppressionCounterCap) >= p_def.high_threshold;
	return out;
}

SuppressionState mirror_suppression(
		const SuppressionDef &p_def, const SuppressionState &p_mirror, uint16_t p_counter, Tick p_hit_tick) {
	if (p_counter == 0) {
		return SuppressionState();
	}
	if (p_counter == p_mirror.counter && p_hit_tick == p_mirror.hit_tick) {
		return p_mirror;
	}
	SuppressionState out;
	out.counter = p_counter;
	out.hit_tick = p_hit_tick;
	out.suppressed = p_def.present &&
			(suppressed(p_def, p_mirror, p_hit_tick) ||
					float(p_counter) / float(kSuppressionCounterCap) >= p_def.high_threshold);
	return out;
}

std::optional<NearMiss> near_miss(
		const Vec3 &p_from, const Vec3 &p_to, const Vec3 &p_origin, const Vec3 &p_centre, float p_radius) {
	const Vec3 d = core::vec::sub(p_to, p_from);
	const float length2 = core::vec::dot(d, d);
	if (!(length2 > 0.0f)) {
		return std::nullopt;
	}
	const float along = core::vec::dot(core::vec::sub(p_centre, p_from), d) / length2;
	if (!(along >= 0.0f)) {
		return std::nullopt;
	}
	const Vec3 closest = core::vec::add(p_from, core::vec::scale(d, std::min(along, 1.0f)));
	const float miss = core::vec::length(core::vec::sub(closest, p_centre));
	if (!(miss < p_radius)) {
		return std::nullopt;
	}
	return NearMiss{ core::vec::length(core::vec::sub(closest, p_origin)), miss };
}

uint32_t near_miss_amount(const ProjectileSimDef &p_projectile, const SuppressionDef &p_def, const NearMiss &p_miss) {
	if (!(p_def.sphere_radius > 0.0f)) {
		return 0;
	}
	const float lo = p_projectile.suppression_min_distance;
	const float span = p_projectile.suppression_max_distance - lo;
	const float f = span > 0.0f ? std::clamp((p_miss.travelled - lo) / span, 0.0f, 1.0f)
								: (p_miss.travelled > lo ? 1.0f : 0.0f);
	const float weight = (1.0f - f) * p_projectile.suppression_max + f * p_projectile.suppression_min;
	const float radial = 1.0f - p_def.decay * p_miss.miss / p_def.sphere_radius;
	const float amount = weight * radial * kSuppressionAmountScale;
	if (!(amount >= 1.0f)) {
		return 0;
	}
	return uint32_t(std::min(amount, float(kSuppressionCounterCap)));
}

uint32_t near_miss_add(uint32_t p_amount, float p_victim_resist, float p_shooter_boost) {
	const double add = double(p_amount) * double(p_victim_resist) * double(p_shooter_boost) + 0.5;
	if (!(add >= 1.0)) {
		return 0;
	}
	return uint32_t(std::min(add, double(kSuppressionCounterCap)));
}

Dispersion suppressed_dispersion(const Dispersion &p_dispersion, const SwayModifier &p_increments, float p_scale) {
	if (!(p_scale > 0.0f)) {
		return p_dispersion;
	}
	Dispersion d = p_dispersion;
	d.min_angle += p_scale * p_increments.min_angle;
	d.max_angle += p_scale * p_increments.max_angle;
	d.increase_per_shot += p_scale * p_increments.increase_per_shot;
	d.decrease_per_second += p_scale * p_increments.decrease_per_second;
	return d;
}
}
