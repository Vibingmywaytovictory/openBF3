#include "bf3/sim/vehicles/vehicle_damage.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bf3::sim::vehicles {

namespace {

constexpr float kDegreesPerRadian = 57.29577951f;

size_t zone_index(HullZone p_zone) {
	return std::min(size_t(p_zone), kHullZoneCount - 1);
}

bool face_axis_z(HullZone p_zone) {
	return p_zone == HullZone::Front || p_zone == HullZone::Rear;
}

void heal_zones(const VehicleDamageRule &p_rule, VehicleHealth &r_state, float p_amount) {
	const float full = full_vehicle_health(p_rule);
	if (!(full > 0.0f)) {
		return;
	}
	for (size_t i = 0; i < kHullZoneCount; ++i) {
		const float max = p_rule.zones[i].max_health;
		if (max > 0.0f) {
			r_state.health[i] = std::clamp(r_state.health[i] + p_amount * max / full, 0.0f, max);
		}
	}
}

VehicleHit plain_hit(float p_damage) {
	VehicleHit h;
	h.damage = p_damage;
	h.zone = HullZone::Front;
	h.modifiers = false;
	return h;
}

}

float angle_of_impact_factor(const AngleOfImpactRule &p_rule, float p_degrees) {
	if (!p_rule.enabled) {
		return 1.0f;
	}
	if (p_degrees >= p_rule.zone23_degrees) {
		return p_rule.multiplier[2];
	}
	if (p_degrees >= p_rule.zone12_degrees) {
		return p_rule.multiplier[1];
	}
	return p_rule.multiplier[0];
}

float zone_angle_factor(const HealthZoneRule &p_zone, float p_degrees) {
	return std::max(p_degrees, p_zone.min_damage_angle) / kZoneAngleScaleDegrees * p_zone.damage_angle_multiplier;
}

float damage_modifier(float p_armor_multiplier, const DamageParties &p_parties) {
	float m = 1.0f;
	if (p_parties.difficulty && p_parties.attacker_is_player && p_parties.attacker_team != 0 &&
			p_parties.attacker_team == p_parties.vehicle_team && !p_parties.attacker_is_occupant &&
			!p_parties.friendly_fire) {
		m = p_parties.difficulty->friendly_damage_modifier;
	}
	if (!(p_armor_multiplier > 0.0f)) {
		return 0.0f;
	}
	m /= p_armor_multiplier;
	if (p_parties.human_occupant) {
		if (p_parties.difficulty) {
			m *= p_parties.difficulty->vehicle_damage_modifier;
		}
		if (!p_parties.friendly_fire && p_parties.attacker_is_occupant) {
			m = 0.0f;
		}
	}
	return m;
}

float full_vehicle_health(const VehicleDamageRule &p_rule) {
	float full = 0.0f;
	for (const HealthZoneRule &z : p_rule.zones) {
		full = std::max(full, z.max_health);
	}
	return full;
}

float vehicle_health(const VehicleDamageRule &p_rule, const VehicleHealth &p_state) {
	float ratio = std::numeric_limits<float>::infinity();
	for (size_t i = 0; i < kHullZoneCount; ++i) {
		const float max = p_rule.zones[i].max_health;
		if (max > 0.0f) {
			ratio = std::min(ratio, p_state.health[i] / max);
		}
	}
	return std::isfinite(ratio) ? full_vehicle_health(p_rule) * ratio : 0.0f;
}

VehicleHealth spawned_vehicle_health(const VehicleDamageRule &p_rule) {
	VehicleHealth s;
	for (size_t i = 0; i < kHullZoneCount; ++i) {
		s.health[i] = p_rule.zones[i].max_health;
		s.shield[i] = p_rule.zones[i].max_shield;
	}
	return s;
}

HullHit hull_hit(const HullBox &p_box, const Vec3 &p_local_point, const Vec3 &p_local_direction) {
	HullHit out;
	Vec3 n{};
	for (size_t k = 0; k < 3; ++k) {
		n[k] = p_box.half[k] > 0.0f ? (p_local_point[k] - p_box.centre[k]) / p_box.half[k] : 0.0f;
	}
	const bool b0 = n[0] - n[2] > 0.0f;
	const bool b1 = -n[0] - n[2] > 0.0f;
	out.zone = b0 ? (b1 ? HullZone::Rear : HullZone::Right) : (b1 ? HullZone::Left : HullZone::Front);

	const float length = std::sqrt(p_local_direction[0] * p_local_direction[0] +
			p_local_direction[1] * p_local_direction[1] + p_local_direction[2] * p_local_direction[2]);
	if (!(length > 0.0f)) {
		return out;
	}
	const Vec3 d{ p_local_direction[0] / length, p_local_direction[1] / length, p_local_direction[2] / length };
	const float reach = 4.0f * (p_box.half[0] + p_box.half[1] + p_box.half[2]) +
			std::fabs(p_local_point[0] - p_box.centre[0]) + std::fabs(p_local_point[1] - p_box.centre[1]) +
			std::fabs(p_local_point[2] - p_box.centre[2]);
	float enter = -std::numeric_limits<float>::infinity();
	float leave = std::numeric_limits<float>::infinity();
	int axis = -1;
	float sign = 0.0f;
	bool inside = true;
	for (int k = 0; k < 3; ++k) {
		const float o = p_local_point[k] - d[k] * reach;
		const float lo = p_box.centre[k] - p_box.half[k], hi = p_box.centre[k] + p_box.half[k];
		if (std::fabs(d[k]) < kHorizontalDirectionFloor) {
			inside = inside && o >= lo && o <= hi;
			continue;
		}
		const float t_lo = (lo - o) / d[k], t_hi = (hi - o) / d[k];
		const float t0 = std::min(t_lo, t_hi), t1 = std::max(t_lo, t_hi);
		if (t0 > enter) {
			enter = t0;
			axis = k;
			sign = d[k] > 0.0f ? -1.0f : 1.0f;
		}
		leave = std::min(leave, t1);
	}
	if (!inside || axis < 0 || enter > leave) {
		axis = 0;
		for (int k = 1; k < 3; ++k) {
			if (std::fabs(n[k]) > std::fabs(n[axis])) {
				axis = k;
			}
		}
		sign = n[axis] >= 0.0f ? 1.0f : -1.0f;
	}
	const float cosine = std::clamp(-d[axis] * sign, -1.0f, 1.0f);
	out.impact_degrees = std::acos(cosine) * kDegreesPerRadian;

	const float h = std::sqrt(d[0] * d[0] + d[2] * d[2]);
	if (h >= kHorizontalDirectionFloor) {
		const float along = face_axis_z(out.zone) ? d[2] / h : d[0] / h;
		out.zone_degrees = std::fabs(std::asin(std::clamp(along, -1.0f, 1.0f))) * kDegreesPerRadian;
		out.zone_angle = true;
	}
	return out;
}

VehicleDamageOutcome apply_vehicle_damage(
		const VehicleDamageRule &p_rule, VehicleHealth &r_state, const VehicleHit &p_hit) {
	VehicleDamageOutcome out;
	out.health_before = vehicle_health(p_rule, r_state);
	out.health_after = out.health_before;
	if (!(out.health_before > 0.0f) || !std::isfinite(p_hit.damage)) {
		return out;
	}
	float d = p_hit.damage;
	if (d > 0.0f && p_hit.modifiers) {
		d *= angle_of_impact_factor(p_rule.angle_of_impact, p_hit.impact_degrees) * p_hit.modifier;
	}
	const size_t zi = zone_index(p_hit.zone);
	const HealthZoneRule &z = p_rule.zones[zi];
	if (!(z.max_health > 0.0f)) {
		return out;
	}
	if (d > 0.0f && p_hit.zone_angle && z.use_damage_angle) {
		d *= zone_angle_factor(z, p_hit.zone_degrees);
	}
	out.applied = true;
	out.damage = d;
	if (d > 0.0f) {
		if (z.max_shield > 0.0f && r_state.shield[zi] >= z.max_shield && d >= z.max_shield) {
			r_state.shield[zi] = std::max(0.0f, r_state.shield[zi] - d);
			out.absorbed = true;
			return out;
		}
		r_state.health[zi] = std::max(0.0f, r_state.health[zi] - d);
	} else {
		const float repair = -d;
		const float added = std::min(repair, std::max(0.0f, z.max_health - r_state.health[zi]));
		r_state.health[zi] += added;
		r_state.shield[zi] = std::min(z.max_shield, r_state.shield[zi] + (repair - added));
	}
	if (p_rule.share_damage) {
		for (size_t j = 0; j < kHullZoneCount; ++j) {
			const float max = p_rule.zones[j].max_health;
			if (j != zi && max > 0.0f) {
				r_state.health[j] = std::clamp(r_state.health[j] - max * (d / z.max_health), 0.0f, max);
			}
		}
	}
	r_state.regeneration_timer = 0.0f;
	if (d < 0.0f) {
		r_state.decay_timer = 0.0f;
	}
	out.health_after = vehicle_health(p_rule, r_state);
	const float full = full_vehicle_health(p_rule);
	if (r_state.condition == VehicleCondition::Healthy && p_rule.disabled_threshold > 0.0f &&
			p_rule.disabled_threshold < out.health_before && out.health_after <= p_rule.disabled_threshold) {
		r_state.condition = VehicleCondition::Disabled;
		out.events |= vehicle_events::kDisabled;
	} else if (r_state.condition == VehicleCondition::Disabled && out.health_before < full &&
			out.health_after >= full) {
		r_state.condition = VehicleCondition::Healthy;
		out.events |= vehicle_events::kHealthy | vehicle_events::kFullHealth;
	}
	if (d > 0.0f && out.health_after <= 0.0f && r_state.condition != VehicleCondition::Destroyed) {
		r_state.condition = VehicleCondition::Destroyed;
		r_state.destroyed_timer = 0.0f;
		r_state.explode_now = p_rule.pre_destruction_threshold > 0.0f && d > p_rule.pre_destruction_threshold;
		out.events |= vehicle_events::kDestroyed;
	}
	return out;
}

VehicleTickOutcome step_vehicle_health(
		const VehicleDamageRule &p_rule, VehicleHealth &r_state, const VehicleTickInput &p_input) {
	VehicleTickOutcome out;
	const float dt = p_input.dt;
	if (r_state.condition == VehicleCondition::Destroyed) {
		if (!r_state.exploded) {
			r_state.destroyed_timer += dt;
			const float window = std::max(p_rule.pre_explosion_time, p_rule.critical_damage_time);
			if (r_state.explode_now || r_state.destroyed_timer >= window) {
				r_state.exploded = true;
				out.events |= vehicle_events::kExploded;
			}
		}
		return out;
	}
	if (p_input.abandon_applies) {
		if (!p_input.abandoned) {
			r_state.abandon_timer = p_input.time_until_abandoned;
		} else {
			r_state.abandon_timer -= dt;
			if (r_state.abandon_timer <= kAbandonMinimumLeft) {
				const float left = p_input.time_until_abandoned_is_destroyed + r_state.abandon_timer;
				const float health = vehicle_health(p_rule, r_state);
				out.abandon_damage = left <= kAbandonMinimumLeft ? health : health * dt / left;
				out.events |= apply_vehicle_damage(p_rule, r_state, plain_hit(out.abandon_damage)).events;
			}
		}
	}
	if (r_state.condition == VehicleCondition::Destroyed) {
		return out;
	}
	if (p_rule.upside_down_damage > 0.0f) {
		const float limit = std::cos(p_rule.upside_down_degrees / kDegreesPerRadian);
		if (p_input.up_y < limit) {
			if (!r_state.upside_down) {
				r_state.upside_down = true;
				r_state.upside_down_timer = 0.0f;
			}
			r_state.upside_down_timer += dt;
			if (r_state.upside_down_timer > p_rule.upside_down_delay) {
				out.upside_down_damage = p_rule.upside_down_damage * r_state.upside_down_timer;
				r_state.upside_down_timer = 0.0f;
				out.events |= vehicle_events::kUpsideDownHit |
						apply_vehicle_damage(p_rule, r_state, plain_hit(out.upside_down_damage)).events;
			}
		} else {
			r_state.upside_down = false;
			r_state.upside_down_timer = 0.0f;
		}
	}
	if (r_state.condition == VehicleCondition::Destroyed) {
		return out;
	}
	const float full = full_vehicle_health(p_rule);
	if (r_state.condition == VehicleCondition::Healthy && !p_input.regeneration_disabled &&
			p_rule.regeneration_rate > 0.0f) {
		const float before = r_state.regeneration_timer;
		r_state.regeneration_timer += dt;
		const float health = vehicle_health(p_rule, r_state);
		if (r_state.regeneration_timer >= p_rule.regeneration_delay && health < full) {
			if (before < p_rule.regeneration_delay) {
				out.events |= vehicle_events::kRegenerationStarted;
			}
			out.regenerated = std::clamp(p_rule.regeneration_rate * dt, 0.0f, full - health);
			heal_zones(p_rule, r_state, out.regenerated);
			if (vehicle_health(p_rule, r_state) >= full) {
				out.events |= vehicle_events::kFullHealth;
			}
		}
	}
	if (r_state.condition == VehicleCondition::Disabled && p_rule.decay_rate > 0.0f) {
		r_state.decay_timer += dt;
		if (r_state.decay_timer >= p_rule.decay_delay) {
			out.decay_damage = std::clamp(p_rule.decay_rate * dt, 0.0f, vehicle_health(p_rule, r_state));
			out.events |= apply_vehicle_damage(p_rule, r_state, plain_hit(out.decay_damage)).events;
		}
	}
	return out;
}

}
