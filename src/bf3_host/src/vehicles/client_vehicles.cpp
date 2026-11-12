#include "client_vehicles.h"

#include <algorithm>
#include <cmath>

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
	v.previous_tick = p_tick;
	v.previous = v.pose();
	return v;
}

PresentedVehiclePose PresentedVehicle::pose() const {
	PresentedVehiclePose p;
	p.com_position = com_position;
	p.rotation = rotation;
	p.wheel_angle = wheel_angle;
	p.wheel_steer = wheel_steer;
	p.suspension_length = suspension_length;
	return p;
}

PresentedVehicle follow_vehicle(const PresentedVehicle &p_shown, const PresentedVehicle &p_received) {
	PresentedVehicle v = p_received;
	if (p_shown.generation != p_received.generation) {
		return v;
	}
	if (p_shown.tick < p_received.tick) {
		v.previous_tick = p_shown.tick;
		v.previous = p_shown.pose();
	} else {
		v.previous_tick = p_shown.previous_tick;
		v.previous = p_shown.previous;
	}
	return v;
}

namespace {

float lerp_angle(float p_a, float p_b, float p_t) {
	return p_a + std::remainder(p_b - p_a, 6.28318530717958647692f) * p_t;
}

std::array<float, 4> slerp(const std::array<float, 4> &p_a, std::array<float, 4> p_b, float p_t) {
	float d = p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2] + p_a[3] * p_b[3];
	if (d < 0.0f) {
		for (float &c : p_b) {
			c = -c;
		}
		d = -d;
	}
	std::array<float, 4> out{};
	float n = 0.0f;
	for (size_t i = 0; i < 4; ++i) {
		out[i] = p_a[i] + (p_b[i] - p_a[i]) * p_t;
		n += out[i] * out[i];
	}
	n = n > 0.0f ? 1.0f / std::sqrt(n) : 0.0f;
	for (float &c : out) {
		c *= n;
	}
	return n > 0.0f ? out : p_b;
}

}

std::optional<PresentedSeat> vehicle_seat_of(std::span<const PresentedVehicle> p_vehicles, uint8_t p_slot) {
	if (p_slot == net::kNoVehicleOccupant) {
		return std::nullopt;
	}
	for (const PresentedVehicle &v : p_vehicles) {
		for (uint8_t k = 0; k < v.seat_count && k < v.occupants.size(); ++k) {
			if (v.occupants[k] == p_slot) {
				return PresentedSeat{ v.spawner, k };
			}
		}
	}
	return std::nullopt;
}

PresentedVehiclePose vehicle_pose_at(const PresentedVehicle &p_vehicle, double p_ticks) {
	const PresentedVehiclePose now = p_vehicle.pose();
	if (!std::isfinite(p_ticks)) {
		return now;
	}
	if (p_ticks >= double(p_vehicle.tick)) {
		const float ahead =
				float(std::min(p_ticks - double(p_vehicle.tick), kVehicleExtrapolateTicks) * double(sim::kTickSeconds));
		PresentedVehiclePose out = now;
		for (size_t i = 0; i < 3; ++i) {
			out.com_position[i] += p_vehicle.linear_velocity[i] * ahead;
		}
		return out;
	}
	if (p_vehicle.previous_tick >= p_vehicle.tick || p_ticks <= double(p_vehicle.previous_tick)) {
		return p_vehicle.previous_tick >= p_vehicle.tick ? now : p_vehicle.previous;
	}
	const float t =
			float((p_ticks - double(p_vehicle.previous_tick)) / double(p_vehicle.tick - p_vehicle.previous_tick));
	const PresentedVehiclePose &a = p_vehicle.previous;
	PresentedVehiclePose out = now;
	for (size_t i = 0; i < 3; ++i) {
		out.com_position[i] = a.com_position[i] + (now.com_position[i] - a.com_position[i]) * t;
	}
	out.rotation = slerp(a.rotation, now.rotation, t);
	for (size_t w = 0; w < p_vehicle.wheel_count; ++w) {
		out.wheel_angle[w] = lerp_angle(a.wheel_angle[w], now.wheel_angle[w], t);
		out.wheel_steer[w] = a.wheel_steer[w] + (now.wheel_steer[w] - a.wheel_steer[w]) * t;
		out.suspension_length[w] = a.suspension_length[w] + (now.suspension_length[w] - a.suspension_length[w]) * t;
	}
	return out;
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
			*at = follow_vehicle(*at, present_vehicle(w, m->server_tick));
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
