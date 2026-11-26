#include "bf3/ai/vehicle/vehicle_change.h"

#include "bf3/ai/squad/squad_world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace bf3::ai {

namespace {

bool occupied(const ChangeVehicle &p_vehicle, uint8_t p_seat) {
	return p_vehicle.seat[p_seat].occupant >= 0;
}

float move_urgency(const ChangeBot &p_bot, const ChangeVehicle *p_vehicle, ChangeTarget p_unit, ChangeTarget p_current,
		bool p_other) {
	if (p_unit.soldier()) {
		return p_bot.max_speed * 5.0f;
	}
	if (p_unit.seat == 0) {
		return p_vehicle->max_speed * 5.0f;
	}
	const ChangeTarget primary{ p_unit.vehicle, 0 };
	if (primary == p_current && p_other) {
		return 0.0f;
	}
	if (!occupied(*p_vehicle, 0)) {
		if (p_vehicle->side != p_bot.side) {
			return 0.0f;
		}
		const double since = p_bot.now_s - p_vehicle->seat[0].possessed_s;
		return since < kPossessionWindowSeconds
				? (1.0f - float(since / kPossessionWindowSeconds)) * (p_vehicle->max_speed * 4.0f)
				: 0.0f;
	}
	const ChangeSeat &driver = p_vehicle->seat[0];
	if (driver.occupant_bot && !driver.same_order) {
		return p_vehicle->max_speed * 2.5f;
	}
	return p_vehicle->max_speed * 4.0f;
}

const ChangeStrengths &unit_strengths(const ChangeBot &p_bot, const ChangeVehicle *p_vehicle, ChangeTarget p_unit) {
	return p_unit.soldier() ? p_bot.strengths : p_vehicle->seat[p_unit.seat].strengths;
}

void add_seat_strengths(ChangeStrengths &r_strengths, const ChangeVehicle &p_vehicle, ChangeTarget p_unit,
		ChangeTarget p_current, bool p_other) {
	const auto add = [&](uint8_t p_seat, float p_share) {
		const ChangeTarget seat{ p_unit.vehicle, p_seat };
		if ((seat != p_current || !p_other) && occupied(p_vehicle, p_seat)) {
			for (size_t i = 0; i < kChangeStrengthCount; ++i) {
				r_strengths[i] = p_share * p_vehicle.seat[p_seat].strengths[i] + r_strengths[i];
			}
		}
	};
	float share = 0.4f;
	if (p_unit.seat != 0) {
		add(0, 0.4f);
	} else if (!p_vehicle.seat[0].armed) {
		share = 0.9f;
	}
	for (uint8_t k = 1; k < p_vehicle.seats; ++k) {
		if (k != p_unit.seat) {
			add(k, share);
		}
	}
}

float threat_to(const ChangeStrengths &p_strengths, int p_own, bool p_exposed) {
	return p_exposed ? std::max(p_strengths[size_t(kChangeInfantryStrength)], p_strengths[size_t(p_own)])
					 : p_strengths[size_t(p_own)];
}

bool normalise(Vec3 &r_v) {
	const float length_sq = r_v[0] * r_v[0] + r_v[1] * r_v[1] + r_v[2] * r_v[2];
	if (std::fabs(length_sq - 1.0f) < std::numeric_limits<float>::epsilon()) {
		return true;
	}
	if (std::fabs(length_sq) < std::numeric_limits<float>::epsilon()) {
		return false;
	}
	const float inverse = 1.0f / std::sqrt(length_sq);
	r_v = { r_v[0] * inverse, r_v[1] * inverse, r_v[2] * inverse };
	return true;
}

Vec3 offset(const Vec3 &p_to, const Vec3 &p_from) {
	return { p_to[0] - p_from[0], p_to[1] - p_from[1], p_to[2] - p_from[2] };
}

bool camera_towards(const ChangeVehicle &p_vehicle, uint8_t p_seat, Vec3 p_direction) {
	return normalise(p_direction) && change_camera_valid(p_vehicle, p_seat, p_direction);
}

float fire_strength(const ChangeBot &p_bot, std::span<const ChangeVehicle> p_vehicles, const ChangeVehicle *p_vehicle,
		ChangeTarget p_unit, ChangeTarget p_current, bool p_other) {
	const bool usable = p_unit != p_current || !p_other;
	const bool primary = p_unit.soldier() || p_unit.seat == 0;
	const bool exposed = !p_unit.soldier() && p_vehicle->seat[p_unit.seat].exposed;
	const int own = p_unit.soldier() ? kChangeInfantryStrength : p_vehicle->strength_type;
	const ChangeStrengths &unit = unit_strengths(p_bot, p_vehicle, p_unit);
	ChangeStrengths strengths{};
	if (usable) {
		strengths = unit;
	}
	if (p_vehicle != nullptr) {
		add_seat_strengths(strengths, *p_vehicle, p_unit, p_current, p_other);
	}
	const bool driven = primary || (ChangeTarget{ p_unit.vehicle, 0 } != p_current && occupied(*p_vehicle, 0));
	bool no_mobile = !driven;
	const Vec3 at = p_vehicle != nullptr ? p_vehicle->position : p_bot.position;
	if (p_bot.sensed.empty()) {
		if (!driven) {
			const ChangeSeat &seat = p_vehicle->seat[p_unit.seat];
			if (!seat.armed) {
				return 0.0f;
			}
			const auto in_reach = [&](const ChangeSensed &p_target) {
				return distance_sq(p_target.position, p_bot.position) <= seat.max_range * seat.max_range;
			};
			if (std::any_of(p_bot.known.begin(), p_bot.known.end(), in_reach)) {
				float best = 0.0f;
				float value = 0.0f;
				for (const ChangeSensed &target : p_bot.known) {
					if (!in_reach(target)) {
						continue;
					}
					const float distance = std::sqrt(distance_sq(target.position, at));
					if (!(distance <= std::max(seat.max_range, 50.0f) && std::min(seat.min_range, 0.1f) <= distance)) {
						continue;
					}
					const float s = strengths[size_t(target.strength_type)];
					value = s * s - threat_to(target.strengths, own, exposed);
					best = std::max(best, value);
				}
				return best;
			}
			for (const Vec3 &neighbour : p_bot.neighbour_offsets) {
				if (camera_towards(*p_vehicle, p_unit.seat, neighbour)) {
					no_mobile = false;
				}
			}
			if (!no_mobile) {
				return 0.0f;
			}
		}
	} else {
		const ChangeVehicle *const carrier = p_current.soldier() ? nullptr : &p_vehicles[size_t(p_current.vehicle)];
		const ChangeStrengths &current = unit_strengths(p_bot, carrier, p_current);
		bool engaged = false;
		bool valid = true;
		float best = 0.0f;
		for (const ChangeSensed &target : p_bot.sensed) {
			if (!target.sided) {
				continue;
			}
			if (!driven) {
				valid = camera_towards(*p_vehicle, p_unit.seat, offset(target.position, at));
				no_mobile = no_mobile && !valid;
			}
			const size_t type = size_t(target.strength_type);
			float value = 0.0f;
			if (valid && current[type] <= unit[type]) {
				engaged = true;
				value = strengths[type] * strengths[type] - threat_to(target.strengths, own, exposed);
			}
			best = std::max(value, best);
		}
		if (!driven && no_mobile) {
			return 0.0f;
		}
		if (engaged || !driven) {
			return best * 4.0f;
		}
	}
	float best = 0.0f;
	float running = 0.0f;
	for (size_t i = 0; i < kChangeStrengthCount; ++i) {
		const float weight = p_bot.enemy_types[i];
		const float value = std::max(weight <= 0.5f ? strengths[i] * 6.0f : strengths[i] * 3.0f * weight, running);
		if (0.0f < weight && best < value) {
			best = value;
		}
		running = value;
	}
	if (best <= 0.0f) {
		return running;
	}
	return best - threat_to(p_bot.enemy_strengths, own, exposed);
}

}

bool change_camera_valid(const ChangeVehicle &p_vehicle, uint8_t p_seat, const Vec3 &p_direction) {
	const ChangeSeat &seat = p_vehicle.seat[p_seat];
	const auto dot = [&](const Vec3 &p_axis) {
		return p_direction[0] * p_axis[0] + p_direction[1] * p_axis[1] + p_direction[2] * p_axis[2];
	};
	constexpr float kDegrees = 180.0f / std::numbers::pi_v<float>;
	const float pitch = std::asin(std::clamp(dot(p_vehicle.hull_up), -1.0f, 1.0f)) * kDegrees;
	float yaw = std::atan2(dot(p_vehicle.right), dot(p_vehicle.forward)) * kDegrees - seat.aim_yaw_deg;
	yaw = std::remainder(yaw, 360.0f);
	return seat.yaw_min_deg <= yaw && yaw <= seat.yaw_max_deg && seat.pitch_min_deg <= pitch &&
			pitch <= seat.pitch_max_deg;
}

float change_unit_urgency(
		const ChangeBot &p_bot, std::span<const ChangeVehicle> p_vehicles, ChangeTarget p_unit, bool p_other) {
	const ChangeTarget current{ p_bot.vehicle, p_bot.seat };
	const ChangeVehicle *const vehicle = p_unit.soldier() ? nullptr : &p_vehicles[size_t(p_unit.vehicle)];
	const ChangeVehicle *const carrier = p_bot.vehicle >= 0 ? &p_vehicles[size_t(p_bot.vehicle)] : nullptr;
	if (vehicle != nullptr) {
		const int booked = vehicle->seat[p_unit.seat].booked_by;
		if (booked >= 0 && booked != p_bot.player_id) {
			return 0.0f;
		}
	}
	const ChangeUnitKind kind = vehicle != nullptr ? vehicle->kind : ChangeUnitKind::Soldier;
	if (p_unit != current && kind != ChangeUnitKind::Air) {
		const float altitude = vehicle != nullptr ? vehicle->altitude : carrier != nullptr ? carrier->altitude : 0.0f;
		if (altitude > kChangeMaxAltitude) {
			return 0.0f;
		}
	}
	if (p_other && vehicle != nullptr && occupied(*vehicle, p_unit.seat)) {
		return 0.0f;
	}
	float urgency = move_urgency(p_bot, vehicle, p_unit, current, p_other) +
			fire_strength(p_bot, p_vehicles, vehicle, p_unit, current, p_other);
	if (0.01f < urgency) {
		urgency = (vehicle != nullptr ? vehicle->seat[p_unit.seat].basic : p_bot.basic) * 4.0f + urgency;
	}
	const float health = vehicle != nullptr ? vehicle->health : p_bot.health;
	float bail = p_bot.bail_value;
	if (vehicle != nullptr && p_unit.seat == 0 && p_unit == current) {
		bail *= 0.3f;
	}
	if (!(bail <= health) && vehicle != nullptr) {
		return 0.0f;
	}
	urgency *= health;
	const int side = vehicle != nullptr ? vehicle->side : p_bot.side;
	if (side == p_bot.side) {
		const double since =
				p_bot.now_s - (vehicle != nullptr ? vehicle->seat[p_unit.seat].possessed_s : kNeverPossessedSeconds);
		if (since < kPossessionWindowSeconds) {
			urgency = float(1.0 / kPossessionWindowSeconds * since) * urgency;
		}
	}
	return urgency;
}

}
