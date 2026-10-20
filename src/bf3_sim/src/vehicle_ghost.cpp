#include "bf3/sim/vehicle_ghost.h"

#include "bf3/sim/vehicle_model.h"

#include <cmath>
#include <string>

namespace bf3::sim {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kInvSqrt2 = 0.70710678118654752440084436210485;
constexpr unsigned kMaxOrientationBits = 16;

uint32_t max_code(unsigned p_bits) {
	return p_bits >= 32 ? 0xFFFFFFFFu : ((1u << p_bits) - 1u);
}

unsigned orientation_bits(const VehicleGhostQuantisation &p_q) {
	return p_q.orientation_bits < 2 ? 2 : (p_q.orientation_bits > kMaxOrientationBits ? kMaxOrientationBits : p_q.orientation_bits);
}

GhostQuantizer suspension_quantizer(float p_max, unsigned p_bits) {
	GhostQuantizer q;
	q.min = 0.0f;
	q.bits = p_bits;
	q.step = p_max > 0.0f && p_bits > 0 ? float(double(p_max) / double(max_code(p_bits))) : 1.0f;
	return q;
}

uint32_t encode_angle(float p_angle, unsigned p_bits) {
	if (p_bits == 0 || !std::isfinite(p_angle)) {
		return 0;
	}
	const double turns = double(p_angle) / kTwoPi;
	const double whole = std::floor(turns);
	const double fraction = turns - whole;
	const double scale = double(uint64_t(1) << p_bits);
	uint64_t code = uint64_t(fraction * scale + 0.5);
	if (code >= uint64_t(1) << p_bits) {
		code = 0;
	}
	return uint32_t(code);
}

float decode_angle(uint32_t p_code, unsigned p_bits) {
	if (p_bits == 0) {
		return 0.0f;
	}
	return float(double(p_code) * kTwoPi / double(uint64_t(1) << p_bits));
}

uint32_t gear_offset(const VehicleGhostQuantisation &p_q) {
	return p_q.gear_bits == 0 ? 0u : (1u << (p_q.gear_bits - 1));
}

}

size_t vehicle_ghost_bits(const VehicleGhostQuantisation &p_q, size_t p_wheels) {
	size_t bits = 2 * size_t(p_q.position_xz.bits) + p_q.position_y.bits;
	bits += 2 + 3 * size_t(orientation_bits(p_q));
	bits += 3 * size_t(p_q.linear_velocity.bits) + 3 * size_t(p_q.angular_velocity.bits);
	bits += 1;
	const size_t per_wheel = (p_q.send_suspension ? p_q.suspension_bits : 0u) + p_q.wheel_spin.bits + p_q.wheel_angle_bits;
	bits += p_wheels * per_wheel;
	bits += p_q.engine_rpm.bits + p_q.gear_bits + p_q.clutch.bits + 3 * size_t(p_q.gearbox_timer.bits);
	if (p_q.send_controller_inputs) {
		bits += 4 * size_t(p_q.driver_filter.bits);
	}
	bits += 3 * size_t(p_q.driver_filter.bits) + p_q.stand_still_bits + 1;
	return bits;
}

VehicleGhostScale vehicle_ghost_scale(const VehicleModel &p_model) {
	VehicleGhostScale s;
	s.wheel_count = p_model.wheel_count;
	for (size_t i = 0; i < p_model.wheel_count && i < kMaxVehicleWheels; ++i) {
		s.suspension_max[i] = p_model.wheels[i].suspension_max;
	}
	s.min_gear = -int32_t(p_model.reverse_gears);
	s.max_gear = int32_t(p_model.forward_gears);
	return s;
}

void encode_quat_smallest_three(const std::array<float, 4> &p_q, unsigned p_bits, uint8_t &r_largest, uint16_t r_codes[3]) {
	const unsigned bits = p_bits < 2 ? 2 : (p_bits > kMaxOrientationBits ? kMaxOrientationBits : p_bits);
	uint8_t largest = 0;
	double largest_abs = -1.0;
	for (uint8_t i = 0; i < 4; ++i) {
		const double a = std::fabs(double(p_q[i]));
		if (a > largest_abs) {
			largest_abs = a;
			largest = i;
		}
	}
	const double sign = p_q[largest] < 0.0f ? -1.0 : 1.0;
	const double step = 2.0 * kInvSqrt2 / double(max_code(bits));
	size_t k = 0;
	for (uint8_t i = 0; i < 4; ++i) {
		if (i == largest) {
			continue;
		}
		const double c = sign * double(p_q[i]);
		const double t = (c + kInvSqrt2) / step + 0.5;
		uint32_t code = 0;
		if (t > 0.0) {
			code = t >= double(max_code(bits)) ? max_code(bits) : uint32_t(t);
		}
		r_codes[k++] = uint16_t(code);
	}
	r_largest = largest;
}

core::Result<std::array<float, 4>> decode_quat_smallest_three(uint8_t p_largest, const uint16_t p_codes[3], unsigned p_bits) {
	if (p_bits < 2 || p_bits > kMaxOrientationBits) {
		return core::make_error("ghost orientation: " + std::to_string(p_bits) + " bits per component (2..16)");
	}
	if (p_largest > 3) {
		return core::make_error("ghost orientation: largest component index " + std::to_string(p_largest));
	}
	const double step = 2.0 * kInvSqrt2 / double(max_code(p_bits));
	double c[4] = {};
	double sum = 0.0;
	size_t k = 0;
	for (uint8_t i = 0; i < 4; ++i) {
		if (i == p_largest) {
			continue;
		}
		if (p_codes[k] > max_code(p_bits)) {
			return core::make_error("ghost orientation: code " + std::to_string(p_codes[k]) + " exceeds " + std::to_string(p_bits) + " bits");
		}
		c[i] = -kInvSqrt2 + double(p_codes[k]) * step;
		sum += c[i] * c[i];
		++k;
	}
	double radicand = 1.0 - sum;
	if (radicand < -1e-3) {
		return core::make_error("ghost orientation: the three components' squared sum " + std::to_string(sum) + " exceeds 1");
	}
	if (radicand < 0.0) {
		radicand = 0.0;
	}
	c[p_largest] = std::sqrt(radicand);
	const double norm = std::sqrt(sum + radicand);
	std::array<float, 4> out{};
	for (size_t i = 0; i < 4; ++i) {
		out[i] = float(c[i] / norm);
	}
	return out;
}

VehicleGhostCodes encode_vehicle_ghost(const VehicleState &p_state, const VehicleGhostQuantisation &p_q, const VehicleGhostScale &p_scale) {
	VehicleGhostCodes g;
	const VehicleBodyState &b = p_state.body;
	g.position[0] = p_q.position_xz.encode(b.com_position[0]);
	g.position[1] = p_q.position_y.encode(b.com_position[1]);
	g.position[2] = p_q.position_xz.encode(b.com_position[2]);
	encode_quat_smallest_three(b.rotation, orientation_bits(p_q), g.orientation_largest, g.orientation);
	for (size_t i = 0; i < 3; ++i) {
		g.linear_velocity[i] = p_q.linear_velocity.encode(b.linear_velocity[i]);
		g.angular_velocity[i] = p_q.angular_velocity.encode(b.angular_velocity[i]);
	}
	g.active = b.active != 0 ? 1 : 0;
	const size_t wheels = p_state.wheel_count < kMaxVehicleWheels ? p_state.wheel_count : kMaxVehicleWheels;
	g.wheel_count = uint8_t(wheels);
	for (size_t i = 0; i < wheels; ++i) {
		const VehicleWheelState &w = p_state.wheels[i];
		g.suspension[i] = p_q.send_suspension ? suspension_quantizer(p_scale.suspension_max[i], p_q.suspension_bits).encode(w.suspension_length) : 0u;
		g.wheel_spin[i] = p_q.wheel_spin.encode(w.angular_velocity);
		g.wheel_angle[i] = encode_angle(w.angle, p_q.wheel_angle_bits);
	}
	const VehicleControllerState &c = p_state.controller;
	g.engine_rpm = p_q.engine_rpm.encode(c.engine_rpm);
	const int64_t gear = int64_t(c.gear) + int64_t(gear_offset(p_q));
	g.gear = gear < 0 ? 0u : (gear > int64_t(max_code(p_q.gear_bits)) ? max_code(p_q.gear_bits) : uint32_t(gear));
	g.clutch = p_q.clutch.encode(c.clutch);
	g.gearbox_timers[0] = p_q.gearbox_timer.encode(c.gear_switch_time_left);
	g.gearbox_timers[1] = p_q.gearbox_timer.encode(c.clutch_release_time_left);
	g.gearbox_timers[2] = p_q.gearbox_timer.encode(c.gear_switch_latency_time_left);
	if (p_q.send_controller_inputs) {
		g.controller_inputs[0] = p_q.driver_filter.encode(c.forward_input);
		g.controller_inputs[1] = p_q.driver_filter.encode(c.right_input);
		g.controller_inputs[2] = p_q.driver_filter.encode(c.brake_input);
		g.controller_inputs[3] = p_q.driver_filter.encode(c.hand_brake_input);
	}
	const VehicleDriverState &d = p_state.driver;
	g.steer_filter = p_q.driver_filter.encode(d.steer_filter);
	g.throttle_filter = p_q.driver_filter.encode(d.throttle_filter);
	g.brake_filter = p_q.driver_filter.encode(d.brake_filter);
	g.stand_still = d.stand_still_ticks > max_code(p_q.stand_still_bits) ? max_code(p_q.stand_still_bits) : d.stand_still_ticks;
	g.reversing = d.reversing != 0 ? 1 : 0;
	return g;
}

core::Status decode_vehicle_ghost(const VehicleGhostCodes &p_codes, const VehicleGhostQuantisation &p_q, const VehicleGhostScale &p_scale,
		VehicleState &r_state) {
	const auto refuse = [](const std::string &p_what) { return core::make_error("vehicle ghost: " + p_what); };
	const auto within = [](uint32_t p_code, uint32_t p_max) { return p_code <= p_max; };
	if (p_codes.wheel_count > kMaxVehicleWheels || p_scale.wheel_count > kMaxVehicleWheels) {
		return refuse("wheel count " + std::to_string(p_codes.wheel_count) + " exceeds " + std::to_string(kMaxVehicleWheels));
	}
	if (p_codes.wheel_count != p_scale.wheel_count) {
		return refuse("wheel count " + std::to_string(p_codes.wheel_count) + ", the model has " + std::to_string(p_scale.wheel_count));
	}
	if (r_state.wheel_count != p_scale.wheel_count) {
		return refuse("the target state has " + std::to_string(r_state.wheel_count) + " wheels, the model " + std::to_string(p_scale.wheel_count));
	}
	if (!within(p_codes.position[0], p_q.position_xz.max_code()) || !within(p_codes.position[1], p_q.position_y.max_code()) ||
			!within(p_codes.position[2], p_q.position_xz.max_code())) {
		return refuse("position code out of range");
	}
	for (size_t i = 0; i < 3; ++i) {
		if (!within(p_codes.linear_velocity[i], p_q.linear_velocity.max_code()) || !within(p_codes.angular_velocity[i], p_q.angular_velocity.max_code())) {
			return refuse("velocity code out of range");
		}
	}
	if (p_codes.active > 1 || p_codes.reversing > 1) {
		return refuse("a one-bit field above 1");
	}
	for (size_t i = 0; i < p_codes.wheel_count; ++i) {
		if ((p_q.send_suspension && !within(p_codes.suspension[i], max_code(p_q.suspension_bits))) || !within(p_codes.wheel_spin[i], p_q.wheel_spin.max_code()) ||
				!within(p_codes.wheel_angle[i], max_code(p_q.wheel_angle_bits))) {
			return refuse("wheel " + std::to_string(i) + " code out of range");
		}
		if (!std::isfinite(p_scale.suspension_max[i]) || p_scale.suspension_max[i] < 0.0f) {
			return refuse("wheel " + std::to_string(i) + " has no suspension length in the scale");
		}
	}
	if (!within(p_codes.engine_rpm, p_q.engine_rpm.max_code()) || !within(p_codes.gear, max_code(p_q.gear_bits)) ||
			!within(p_codes.clutch, p_q.clutch.max_code())) {
		return refuse("controller code out of range");
	}
	for (const uint32_t t : p_codes.gearbox_timers) {
		if (!within(t, p_q.gearbox_timer.max_code())) {
			return refuse("gearbox timer code out of range");
		}
	}
	if (p_q.send_controller_inputs) {
		for (const uint32_t c : p_codes.controller_inputs) {
			if (!within(c, p_q.driver_filter.max_code())) {
				return refuse("controller input code out of range");
			}
		}
	}
	if (!within(p_codes.steer_filter, p_q.driver_filter.max_code()) || !within(p_codes.throttle_filter, p_q.driver_filter.max_code()) ||
			!within(p_codes.brake_filter, p_q.driver_filter.max_code()) || !within(p_codes.stand_still, max_code(p_q.stand_still_bits))) {
		return refuse("driver code out of range");
	}
	const int32_t gear = int32_t(p_codes.gear) - int32_t(gear_offset(p_q));
	if (gear < p_scale.min_gear || gear > p_scale.max_gear) {
		return refuse("gear " + std::to_string(gear) + " outside " + std::to_string(p_scale.min_gear) + ".." + std::to_string(p_scale.max_gear));
	}
	core::Result<std::array<float, 4>> rotation = decode_quat_smallest_three(p_codes.orientation_largest, p_codes.orientation, orientation_bits(p_q));
	if (!rotation) {
		return core::error_at("vehicle ghost", rotation.error());
	}

	VehicleBodyState &b = r_state.body;
	b.com_position = { p_q.position_xz.decode(p_codes.position[0]), p_q.position_y.decode(p_codes.position[1]), p_q.position_xz.decode(p_codes.position[2]) };
	b.rotation = *rotation;
	for (size_t i = 0; i < 3; ++i) {
		b.linear_velocity[i] = p_q.linear_velocity.decode(p_codes.linear_velocity[i]);
		b.angular_velocity[i] = p_q.angular_velocity.decode(p_codes.angular_velocity[i]);
	}
	b.active = p_codes.active;
	for (size_t i = 0; i < p_codes.wheel_count; ++i) {
		VehicleWheelState &w = r_state.wheels[i];
		if (p_q.send_suspension) {
			w.suspension_length = suspension_quantizer(p_scale.suspension_max[i], p_q.suspension_bits).decode(p_codes.suspension[i]);
		}
		w.angular_velocity = p_q.wheel_spin.decode(p_codes.wheel_spin[i]);
		w.angle = decode_angle(p_codes.wheel_angle[i], p_q.wheel_angle_bits);
	}
	VehicleControllerState &c = r_state.controller;
	c.engine_rpm = p_q.engine_rpm.decode(p_codes.engine_rpm);
	c.gear = gear;
	c.clutch = p_q.clutch.decode(p_codes.clutch);
	c.gear_switch_time_left = p_q.gearbox_timer.decode(p_codes.gearbox_timers[0]);
	c.clutch_release_time_left = p_q.gearbox_timer.decode(p_codes.gearbox_timers[1]);
	c.gear_switch_latency_time_left = p_q.gearbox_timer.decode(p_codes.gearbox_timers[2]);
	if (p_q.send_controller_inputs) {
		c.forward_input = p_q.driver_filter.decode(p_codes.controller_inputs[0]);
		c.right_input = p_q.driver_filter.decode(p_codes.controller_inputs[1]);
		c.brake_input = p_q.driver_filter.decode(p_codes.controller_inputs[2]);
		c.hand_brake_input = p_q.driver_filter.decode(p_codes.controller_inputs[3]);
	}
	VehicleDriverState &d = r_state.driver;
	d.steer_filter = p_q.driver_filter.decode(p_codes.steer_filter);
	d.throttle_filter = p_q.driver_filter.decode(p_codes.throttle_filter);
	d.brake_filter = p_q.driver_filter.decode(p_codes.brake_filter);
	d.stand_still_ticks = uint16_t(p_codes.stand_still);
	d.reversing = p_codes.reversing;
	return core::Status::success();
}

}
