#include "bf3/ai/vehicle/vehicle_change.h"

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
		const double since = p_bot.now_s - p_vehicle->possessed_s;
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
	float urgency = move_urgency(p_bot, vehicle, p_unit, current, p_other);
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
		const double since = p_bot.now_s - (vehicle != nullptr ? vehicle->possessed_s : p_bot.spawned_s);
		if (since < kPossessionWindowSeconds) {
			urgency = float(1.0 / kPossessionWindowSeconds * since) * urgency;
		}
	}
	return urgency;
}

}
