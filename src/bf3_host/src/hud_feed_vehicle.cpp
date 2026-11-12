#include "bf3/host/hud_feed.h"
#include "bf3/host/level_vehicles.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace ui = bf3::core::ui;

namespace {

inline constexpr int32_t kPassengerSquad = 0;
inline constexpr int32_t kPassengerFriend = 1;
inline constexpr int32_t kPassengerEmpty = 2;

const net::PlayerRow *row_of(std::span<const net::PlayerRow> p_players, uint8_t p_slot) {
	for (const net::PlayerRow &row : p_players) {
		if (row.slot == p_slot) {
			return &row;
		}
	}
	return nullptr;
}

ui::HudPassengerSeat seat_kind(const HudFeedInput &p_input, const PresentedVehicle &p_vehicle, size_t p_seat) {
	if (p_seat == 0) {
		return ui::HudPassengerSeat::Driver;
	}
	const LevelVehicles *level = p_input.level_vehicles;
	if (level == nullptr || p_vehicle.spawner >= level->spawners.size()) {
		return ui::HudPassengerSeat::Idle;
	}
	const LevelVehicleKind &kind = level->kinds[level->spawners[p_vehicle.spawner].kind];
	return p_seat < kind.seat_defs.size() && !kind.seat_defs[p_seat].weapons.empty() ? ui::HudPassengerSeat::Turret
																					 : ui::HudPassengerSeat::Idle;
}

}

ui::HudVehicle hud_vehicle(const HudFeedInput &p_input) {
	ui::HudVehicle out;
	if (!p_input.alive || p_input.own_slot == ui::kHudNoPlayer) {
		return out;
	}
	const std::optional<PresentedSeat> seat = vehicle_seat_of(p_input.vehicles, p_input.own_slot);
	if (!seat) {
		return out;
	}
	const auto vehicle = std::find_if(p_input.vehicles.begin(), p_input.vehicles.end(),
			[&](const PresentedVehicle &v) { return v.spawner == seat->spawner; });
	if (vehicle == p_input.vehicles.end()) {
		return out;
	}
	out.in_vehicle = true;
	out.health = std::isfinite(vehicle->health) ? std::clamp(vehicle->health, 0.0f, 1.0f) * 100.0f : 0.0f;
	out.damage_state_disabled = vehicle->condition != sim::vehicles::VehicleCondition::Healthy;
	const net::PlayerRow *own = row_of(p_input.players, p_input.own_slot);
	const uint8_t own_squad = own != nullptr ? own->squad : uint8_t(0);
	const size_t seats = std::min<size_t>(vehicle->seat_count, ui::kHudMaxPassengers);
	for (size_t k = 0; k < seats; ++k) {
		ui::HudPassenger &p = out.passenger_list.passengers[out.passenger_list.count++];
		p.seat = seat_kind(p_input, *vehicle, k);
		const uint8_t occupant = vehicle->occupants[k];
		if (occupant == net::kNoVehicleOccupant) {
			p.team_state = kPassengerEmpty;
			continue;
		}
		const net::PlayerRow *row = row_of(p_input.players, occupant);
		p.is_player = occupant == p_input.own_slot;
		p.team_state = p.is_player || (own_squad != 0 && row != nullptr && row->squad == own_squad) ? kPassengerSquad
																									: kPassengerFriend;
		if (row != nullptr) {
			const size_t n = std::min<size_t>({ row->name_bytes, row->name.size(), p.name.size() - 1 });
			std::copy_n(row->name.begin(), n, p.name.begin());
		}
	}
	return out;
}

}
