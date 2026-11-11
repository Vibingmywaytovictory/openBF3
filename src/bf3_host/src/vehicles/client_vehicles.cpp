#include "client_vehicles.h"

#include <algorithm>

namespace bf3::host {

PresentedVehicle present_vehicle(const net::VehicleWire &p_wire, sim::Tick p_tick) {
	PresentedVehicle v;
	v.spawner = p_wire.spawner;
	v.generation = p_wire.generation;
	v.team = p_wire.team;
	v.seat_count = p_wire.seat_count;
	v.occupants = p_wire.occupants;
	v.tick = p_tick;
	v.com_position = p_wire.state.body.com_position;
	v.rotation = p_wire.state.body.rotation;
	v.linear_velocity = p_wire.state.body.linear_velocity;
	v.wheel_count = std::min<uint8_t>(p_wire.state.wheel_count, uint8_t(sim::kMaxVehicleWheels));
	for (size_t i = 0; i < v.wheel_count; ++i) {
		v.wheel_angle[i] = p_wire.state.wheels[i].angle;
		v.wheel_steer[i] = p_wire.state.wheels[i].steer_angle;
		v.suspension_length[i] = p_wire.state.wheels[i].suspension_length;
	}
	v.engine_rpm = p_wire.state.driver.engine_rpm;
	v.gear = int8_t(p_wire.state.driver.gearbox.gear);
	return v;
}

void ClientVehicles::receive(std::span<const uint8_t> p_bytes) {
	core::Result<net::VehicleStates> m = net::decode_vehicle_states(p_bytes);
	if (!m) {
		++bad_;
		return;
	}
	++messages_;
	newest_ = std::max(newest_, m->server_tick);
	for (const net::VehicleWire &w : m->vehicles) {
		auto at = std::find_if(
				vehicles_.begin(), vehicles_.end(), [&](const PresentedVehicle &v) { return v.spawner == w.spawner; });
		if (at == vehicles_.end()) {
			if (vehicles_.size() >= kMaxPresentedVehicles) {
				++bad_;
				continue;
			}
			vehicles_.push_back(present_vehicle(w, m->server_tick));
		} else if (m->server_tick >= at->tick) {
			*at = present_vehicle(w, m->server_tick);
		}
	}
	std::erase_if(vehicles_, [&](const PresentedVehicle &v) { return newest_ - v.tick > kVehicleStaleTicks; });
}

void ClientVehicles::publish(PresentationFrame &r_frame) const {
	r_frame.vehicle_count = uint8_t(std::min(vehicles_.size(), r_frame.vehicles.size()));
	std::copy_n(vehicles_.begin(), r_frame.vehicle_count, r_frame.vehicles.begin());
}

void ClientVehicles::stats(ClientStats &r_stats) const {
	r_stats.vehicle_messages = messages_;
	r_stats.bad_vehicle_messages = bad_;
}

}
