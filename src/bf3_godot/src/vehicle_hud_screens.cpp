#include "vehicle_hud_screens.h"

#include "native_screens.h"

#include "bf3/host/ui_flow_session.h"

#include <algorithm>

namespace bf3::godot_side {

host::VehicleHudView vehicle_hud_view(const VehicleHudStepInput &p_in) {
	host::VehicleHudView out;
	if (!p_in.alive || p_in.level == nullptr) {
		return out;
	}
	const std::optional<host::PresentedSeat> seat = host::vehicle_seat_of(p_in.vehicles, p_in.own_slot);
	if (!seat || seat->spawner >= p_in.level->spawners.size()) {
		return out;
	}
	const auto vehicle = std::find_if(p_in.vehicles.begin(), p_in.vehicles.end(),
			[&](const host::PresentedVehicle &v) { return v.spawner == seat->spawner; });
	const uint16_t kind_index = p_in.level->spawners[seat->spawner].kind;
	if (vehicle == p_in.vehicles.end() || kind_index >= p_in.level->kinds.size()) {
		return out;
	}
	const host::LevelVehicleKind &kind = p_in.level->kinds[kind_index];
	out.hud = &kind.hud;
	out.vehicle = (uint64_t(seat->spawner) << 16) | (uint64_t(vehicle->generation) << 8) | uint64_t(seat->seat);
	if (seat->seat < kind.seat_defs.size()) {
		if (const core::gameplay::VehicleCameraDef *c =
						host::vehicle_seat_camera(kind.seat_defs[seat->seat], p_in.camera)) {
			out.camera = c->hud_camera;
			out.alternate_view = p_in.alternate_view;
		}
	}
	return out;
}

void VehicleHudScreens::draw(Bf3UiScreens &r_screens) {
	for (const std::string_view p : kVehicleHudDrawnPartitions) {
		r_screens.set_drawn(p, true);
	}
}

size_t VehicleHudScreens::step(
		host::UiFlowSession *p_session, Bf3UiScreens *p_screens, const VehicleHudStepInput &p_in) {
	if (p_session == nullptr || p_screens == nullptr) {
		return 0;
	}
	if (p_session != session_ || p_session->stats().reloads != reloads_) {
		session_ = p_session;
		reloads_ = p_session->stats().reloads;
		switches_ = host::VehicleHudSwitches();
	}
	const host::VehicleHudView view = vehicle_hud_view(p_in);
	std::string name;
	if (view.hud != nullptr) {
		const std::optional<host::PresentedSeat> seat = host::vehicle_seat_of(p_in.vehicles, p_in.own_slot);
		// INFERRED: the AS2's vehicle name is VehicleEntityData.NameSid, as HelperFunctions.getIsVehicleRussian's twelve
		// names are NameSids. Settle: the binding that feeds the vehicle name traced.
		name = p_in.level->kinds[p_in.level->spawners[seat->spawner].kind].name_sid;
	}
	if (name != vehicle_name_) {
		vehicle_name_ = name;
		p_screens->set_vehicle_name(vehicle_name_);
	}
	return host::apply_vehicle_hud_events(*p_session, switches_.step(view));
}

}
