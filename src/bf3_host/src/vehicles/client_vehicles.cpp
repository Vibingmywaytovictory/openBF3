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
	v.angular_velocity = p_wire.state.body.angular_velocity;
	v.wheel_count = std::min<uint8_t>(p_wire.state.wheel_count, uint8_t(sim::kMaxVehicleWheels));
	for (size_t i = 0; i < v.wheel_count; ++i) {
		v.wheel_angle[i] = p_wire.state.wheels[i].angle;
		v.wheel_spin[i] = p_wire.state.wheels[i].angular_velocity;
		v.wheel_steer[i] = p_wire.state.wheels[i].steer_angle;
		v.suspension_length[i] = p_wire.state.wheels[i].suspension_length;
		v.wheel_contact |= uint8_t((p_wire.state.wheels[i].in_contact != 0 ? 1u : 0u) << i);
	}
	v.throttle = p_wire.state.driver.throttle_filter;
	v.engine_rpm = p_wire.state.driver.engine_rpm;
	v.gear = int8_t(p_wire.state.driver.gearbox.gear);
	v.turret_count = std::min<uint8_t>(p_wire.turret_count, uint8_t(net::kMaxVehicleTurrets));
	std::copy_n(p_wire.turret_angles.begin(), v.turret_count, v.turret_angle.begin());
	v.health = float(p_wire.health) / float(net::kVehicleHealthFull);
	v.condition = sim::vehicles::VehicleCondition(p_wire.condition);
	v.burning = p_wire.burning;
	v.exploded = p_wire.exploded;
	v.window_count = 1;
	v.window[0] = PresentedVehicleSample{ p_tick, v.pose() };
	return v;
}

PresentedVehiclePose PresentedVehicle::pose() const {
	PresentedVehiclePose p;
	p.com_position = com_position;
	p.rotation = rotation;
	p.wheel_angle = wheel_angle;
	p.wheel_steer = wheel_steer;
	p.suspension_length = suspension_length;
	p.turret_angle = turret_angle;
	return p;
}

void present_window(
		PresentedVehicle &r_vehicle, std::span<const PresentedVehicleSample> p_history, double p_render_ticks) {
	size_t first = 0;
	while (first + 1 < p_history.size() && double(p_history[first + 1].tick) <= p_render_ticks) {
		++first;
	}
	r_vehicle.window_count = 0;
	for (size_t k = first; k < p_history.size() && r_vehicle.window_count < kVehicleWindow; ++k) {
		r_vehicle.window[r_vehicle.window_count++] = p_history[k];
	}
}

namespace {

float lerp_angle(float p_a, float p_b, float p_t) {
	return p_a + std::remainder(p_b - p_a, 6.28318530717958647692f) * p_t;
}

std::array<float, 4> turned(const std::array<float, 4> &p_q, const sim::Vec3 &p_omega, float p_seconds) {
	const float h = 0.5f * p_seconds;
	const float x = p_q[0], y = p_q[1], z = p_q[2], w = p_q[3];
	const float wx = p_omega[0], wy = p_omega[1], wz = p_omega[2];
	std::array<float, 4> out{ x + h * (w * wx + wy * z - wz * y), y + h * (w * wy + wz * x - wx * z),
		z + h * (w * wz + wx * y - wy * x), w - h * (wx * x + wy * y + wz * z) };
	const float n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
	if (!(n > 0.0f) || !std::isfinite(n)) {
		return p_q;
	}
	for (float &c : out) {
		c /= n;
	}
	return out;
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

VehicleWreckSight VehicleWrecks::see(const PresentedVehicle &p_vehicle) {
	VehicleWreckSight s;
	if (!p_vehicle.exploded) {
		return s;
	}
	s.hidden = true;
	const auto seen = exploded_.find(p_vehicle.spawner);
	s.explosion = seen == exploded_.end() || seen->second != p_vehicle.generation;
	exploded_[p_vehicle.spawner] = p_vehicle.generation;
	return s;
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
		out.rotation = turned(now.rotation, p_vehicle.angular_velocity, ahead);
		for (size_t w = 0; w < p_vehicle.wheel_count; ++w) {
			out.wheel_angle[w] += p_vehicle.wheel_spin[w] * ahead;
		}
		return out;
	}
	const size_t in_window = std::min<size_t>(p_vehicle.window_count, kVehicleWindow);
	const bool newest_after = in_window == 0 || p_vehicle.window[in_window - 1].tick < p_vehicle.tick;
	const size_t count = in_window + (newest_after ? 1u : 0u);
	const auto tick_of = [&](size_t p_i) { return p_i < in_window ? p_vehicle.window[p_i].tick : p_vehicle.tick; };
	const auto pose_of = [&](size_t p_i) -> const PresentedVehiclePose & {
		return p_i < in_window ? p_vehicle.window[p_i].pose : now;
	};
	if (p_ticks <= double(tick_of(0))) {
		return pose_of(0);
	}
	for (size_t i = 0; i + 1 < count; ++i) {
		const sim::Tick t0 = tick_of(i), t1 = tick_of(i + 1);
		if (p_ticks >= double(t1) || t1 <= t0) {
			continue;
		}
		const float t = float((p_ticks - double(t0)) / double(t1 - t0));
		const PresentedVehiclePose &a = pose_of(i);
		const PresentedVehiclePose &b = pose_of(i + 1);
		PresentedVehiclePose out = b;
		for (size_t k = 0; k < 3; ++k) {
			out.com_position[k] = a.com_position[k] + (b.com_position[k] - a.com_position[k]) * t;
		}
		out.rotation = slerp(a.rotation, b.rotation, t);
		for (size_t w = 0; w < p_vehicle.wheel_count; ++w) {
			out.wheel_angle[w] = lerp_angle(a.wheel_angle[w], b.wheel_angle[w], t);
			out.wheel_steer[w] = a.wheel_steer[w] + (b.wheel_steer[w] - a.wheel_steer[w]) * t;
			out.suspension_length[w] = a.suspension_length[w] + (b.suspension_length[w] - a.suspension_length[w]) * t;
		}
		for (size_t k = 0; k < p_vehicle.turret_count && k < out.turret_angle.size(); ++k) {
			out.turret_angle[k] = lerp_angle(a.turret_angle[k], b.turret_angle[k], t);
		}
		return out;
	}
	return now;
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
				vehicles_.begin(), vehicles_.end(), [&](const Tracked &t) { return t.newest.spawner == w.spawner; });
		if (at == vehicles_.end()) {
			if (vehicles_.size() >= kMaxPresentedVehicles) {
				++bad_;
				continue;
			}
			Tracked t;
			t.newest = present_vehicle(w, m->server_tick);
			t.wire = w;
			t.history.push_back(t.newest.window[0]);
			vehicles_.push_back(std::move(t));
		} else if (m->server_tick >= at->newest.tick) {
			const bool same_life = at->newest.generation == w.generation;
			at->newest = present_vehicle(w, m->server_tick);
			at->wire = w;
			if (!same_life) {
				at->history.clear();
			}
			if (!at->history.empty() && at->history.back().tick == m->server_tick) {
				at->history.back() = at->newest.window[0];
			} else {
				at->history.push_back(at->newest.window[0]);
			}
			while (at->history.size() > kVehicleHistory) {
				at->history.erase(at->history.begin());
			}
		}
	}
	std::erase_if(vehicles_, [&](const Tracked &t) { return newest_ - t.newest.tick > kVehicleStaleTicks; });
}

void ClientVehicles::publish(PresentationFrame &r_frame) const {
	r_frame.vehicle_count = uint8_t(std::min(vehicles_.size(), r_frame.vehicles.size()));
	for (size_t i = 0; i < r_frame.vehicle_count; ++i) {
		r_frame.vehicles[i] = vehicles_[i].newest;
		present_window(r_frame.vehicles[i], vehicles_[i].history, r_frame.render_ticks_at_publish);
	}
}

std::optional<SeenSeat> ClientVehicles::seat_of(uint8_t p_slot) const {
	if (p_slot == net::kNoVehicleOccupant) {
		return std::nullopt;
	}
	for (const Tracked &t : vehicles_) {
		for (uint8_t k = 0; k < t.wire.seat_count && k < t.wire.occupants.size(); ++k) {
			if (t.wire.occupants[k] == p_slot) {
				return SeenSeat{ t.newest.tick, k, &t.wire };
			}
		}
	}
	return std::nullopt;
}

void ClientVehicles::each_wire(const std::function<void(const net::VehicleWire &)> &p_visit) const {
	for (const Tracked &t : vehicles_) {
		p_visit(t.wire);
	}
}

void ClientVehicles::stats(ClientStats &r_stats) const {
	r_stats.vehicle_messages = messages_;
	r_stats.bad_vehicle_messages = bad_;
}

}
