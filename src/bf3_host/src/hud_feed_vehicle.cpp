#include "bf3/host/hud_feed.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/vehicle_hud_flow.h"
#include "bf3/present/vehicle_turrets.h"
#include "bf3/sim/vehicles/vehicle_armament.h"

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

float yaw_deg(const core::Vec3 &p_forward) {
	return std::atan2(p_forward[0], p_forward[2]) / core::kDegToRadF;
}

float pitch_deg(const core::Vec3 &p_forward) {
	return std::asin(std::clamp(p_forward[1], -1.0f, 1.0f)) / core::kDegToRadF;
}

void fill_vehicle_views(const HudFeedInput &p_input, const PresentedVehicle &p_vehicle, const LevelVehicleKind &p_kind,
		size_t p_seat, ui::HudVehicle &r_out) {
	const core::Transform hull = sim::vehicles::hull_transform(p_vehicle.rotation, core::Vec3{});
	r_out.hull_yaw_deg = yaw_deg(hull.forward);
	const float hull_pitch = pitch_deg(hull.forward);
	const size_t angles = std::min<size_t>(p_vehicle.turret_count, p_vehicle.turret_angle.size());
	const std::span<const float> turret_angles(p_vehicle.turret_angle.data(), angles);
	const std::vector<core::Transform> offsets = p_kind.armament != nullptr
			? present::turret_offsets(*p_kind.armament, turret_angles)
			: std::vector<core::Transform>();
	for (size_t entry = 0; entry < r_out.entry_yaw_deg.size(); ++entry) {
		r_out.entry_yaw_deg[entry] = 0.0f;
		if (entry >= p_kind.seat_defs.size()) {
			continue;
		}
		for (const core::gameplay::VehicleCameraDef &c : p_kind.seat_defs[entry].cameras) {
			if (!c.first_person) {
				continue;
			}
			const core::Transform local = c.turret >= 0 && size_t(c.turret) < offsets.size()
					? offsets[size_t(c.turret)].then_inner(c.transform)
					: c.transform;
			r_out.entry_yaw_deg[entry] = yaw_deg(hull.transform_vector(local.forward));
			break;
		}
	}
	const core::gameplay::VehicleCameraDef *camera = p_seat < p_kind.seat_defs.size()
			? vehicle_seat_camera(p_kind.seat_defs[p_seat], p_input.vehicle_camera)
			: nullptr;
	r_out.weapon_orientations = camera != nullptr && camera->hud.use_weapon_orientations;
	if (p_seat < p_kind.seat_defs.size() && !p_kind.seat_defs[p_seat].weapons.empty()) {
		const int weapon = p_kind.seat_defs[p_seat].weapons.front();
		r_out.weapon_item =
				weapon >= 0 && size_t(weapon) < p_kind.weapons.size() ? p_kind.weapons[size_t(weapon)].item_hash : 0u;
	}
	if (!r_out.weapon_orientations || p_kind.armament == nullptr) {
		return;
	}
	sim::vehicles::ArmamentState state{};
	for (size_t k = 0; k < angles && k < state.bodies.size(); ++k) {
		state.bodies[k].angle = std::isfinite(turret_angles[k]) ? turret_angles[k] : 0.0f;
	}
	for (size_t w = 0; w < p_kind.weapons.size() && w < p_kind.armament->weapons.size(); ++w) {
		const std::string &kind = p_kind.weapons[w].classification;
		std::optional<float> *slot = kind == "WCPrimary" ? &r_out.primary_pitch_deg
				: kind == "WCSecondary"					 ? &r_out.secondary_pitch_deg
														 : nullptr;
		if (slot == nullptr || slot->has_value()) {
			continue;
		}
		const core::Transform world = sim::vehicles::weapon_world(*p_kind.armament, state, hull, w);
		*slot = pitch_deg(world.forward) - hull_pitch;
	}
}

void fill_seat_weapon(
		const HudFeedInput &p_input, const LevelVehicleKind &p_kind, size_t p_seat, ui::HudVehicle &r_out) {
	if (!p_input.seat_weapon || p_kind.armament == nullptr) {
		return;
	}
	const net::OwnSeatWeapon &w = *p_input.seat_weapon;
	if (w.weapon >= p_kind.armament->weapons.size() || p_kind.armament->weapons[w.weapon].seat != int(p_seat)) {
		return;
	}
	r_out.seat_weapon = true;
	r_out.seat_weapon_ammo = w.magazine;
	r_out.seat_weapon_heat = net::seat_weapon_heat_of(w.heat);
	r_out.seat_weapon_reloading = w.reloading;
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
	const float speed = std::sqrt(vehicle->linear_velocity.x * vehicle->linear_velocity.x +
			vehicle->linear_velocity.y * vehicle->linear_velocity.y +
			vehicle->linear_velocity.z * vehicle->linear_velocity.z);
	out.speed_kmh = std::isfinite(speed) ? int32_t(std::min(speed * 3.6f, 1.0e6f)) : 0;
	if (const LevelVehicles *level = p_input.level_vehicles; level != nullptr &&
			vehicle->spawner < level->spawners.size() && level->spawners[vehicle->spawner].kind < level->kinds.size()) {
		const LevelVehicleKind &kind = level->kinds[level->spawners[vehicle->spawner].kind];
		const size_t n = std::min(kind.name_sid.size(), out.short_name.size() - 1);
		std::copy_n(kind.name_sid.begin(), n, out.short_name.begin());
		fill_vehicle_views(p_input, *vehicle, kind, seat->seat, out);
		fill_seat_weapon(p_input, kind, seat->seat, out);
	}
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
