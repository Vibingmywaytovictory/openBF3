#include "bf3/sim/vehicle_types.h"

#include <cstddef>
#include <cstring>
#include <string>

namespace bf3::sim {

namespace {

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;

void fnv(uint64_t &r_hash, const void *p_data, size_t p_size) {
	const unsigned char *b = static_cast<const unsigned char *>(p_data);
	for (size_t i = 0; i < p_size; ++i) {
		r_hash ^= b[i];
		r_hash *= kFnvPrime;
	}
}

template <typename State, typename Visit>
bool visit_fields(State &p_state, const Visit &p_visit) {
	auto &b = p_state.body;
	const char *body_names[] = { "body.com_position", "body.rotation", "body.linear_velocity", "body.angular_velocity", "body.force", "body.torque",
		"body.sleep_test_spheres", "body.sleep_test_timer", "body.active", "body.allow_sleeping" };
	const void *body_fields[] = { &b.com_position, b.rotation.data(), &b.linear_velocity, &b.angular_velocity, &b.force,
		&b.torque, b.sleep_test_spheres.data(), &b.sleep_test_timer, &b.active, &b.allow_sleeping };
	const size_t body_sizes[] = { sizeof(b.com_position), sizeof(b.rotation), sizeof(b.linear_velocity), sizeof(b.angular_velocity), sizeof(b.force),
		sizeof(b.torque), sizeof(b.sleep_test_spheres), sizeof(b.sleep_test_timer), sizeof(b.active), sizeof(b.allow_sleeping) };
	for (size_t i = 0; i < sizeof(body_sizes) / sizeof(body_sizes[0]); ++i) {
		if (!p_visit(std::string(body_names[i]), body_fields[i], body_sizes[i])) {
			return false;
		}
	}
	auto &c = p_state.controller;
	const char *controller_names[] = { "controller.forward_input", "controller.right_input", "controller.brake_input", "controller.hand_brake_input",
		"controller.previous_delta_time", "controller.engine_rpm", "controller.gear", "controller.clutch", "controller.gear_switch_time_left",
		"controller.clutch_release_time_left", "controller.gear_switch_latency_time_left" };
	const void *controller_fields[] = { &c.forward_input, &c.right_input, &c.brake_input, &c.hand_brake_input, &c.previous_delta_time, &c.engine_rpm,
		&c.gear, &c.clutch, &c.gear_switch_time_left, &c.clutch_release_time_left, &c.gear_switch_latency_time_left };
	for (size_t i = 0; i < sizeof(controller_fields) / sizeof(controller_fields[0]); ++i) {
		if (!p_visit(std::string(controller_names[i]), controller_fields[i], size_t(4))) {
			return false;
		}
	}
	auto &d = p_state.driver;
	if (!p_visit(std::string("driver.steer_filter"), &d.steer_filter, sizeof(d.steer_filter)) ||
			!p_visit(std::string("driver.throttle_filter"), &d.throttle_filter, sizeof(d.throttle_filter)) ||
			!p_visit(std::string("driver.brake_filter"), &d.brake_filter, sizeof(d.brake_filter)) ||
			!p_visit(std::string("driver.stand_still_ticks"), &d.stand_still_ticks, sizeof(d.stand_still_ticks)) ||
			!p_visit(std::string("driver.reversing"), &d.reversing, sizeof(d.reversing))) {
		return false;
	}
	const size_t wheels = p_state.wheel_count < kMaxVehicleWheels ? p_state.wheel_count : kMaxVehicleWheels;
	for (size_t i = 0; i < wheels; ++i) {
		auto &w = p_state.wheels[i];
		const std::string prefix = "wheels[" + std::to_string(i) + "].";
		const char *wheel_names[] = { "angular_velocity", "angle", "suspension_length", "contact_body", "contact_position", "contact_normal", "contact_lateral",
			"lambda_suspension", "lambda_suspension_max_up", "lambda_longitudinal", "lambda_lateral" };
		const void *wheel_fields[] = { &w.angular_velocity, &w.angle, &w.suspension_length, &w.contact_body,
			&w.contact_position, &w.contact_normal, &w.contact_lateral, &w.lambda_suspension,
			&w.lambda_suspension_max_up, &w.lambda_longitudinal, &w.lambda_lateral };
		const size_t wheel_sizes[] = { 4, 4, 4, 4, 12, 12, 12, 4, 4, 4, 4 };
		for (size_t f = 0; f < sizeof(wheel_sizes) / sizeof(wheel_sizes[0]); ++f) {
			if (!p_visit(prefix + wheel_names[f], wheel_fields[f], wheel_sizes[f])) {
				return false;
			}
		}
	}
	return p_visit(std::string("wheel_count"), &p_state.wheel_count, sizeof(p_state.wheel_count)) &&
			p_visit(std::string("constraint_enabled"), &p_state.constraint_enabled,
					sizeof(p_state.constraint_enabled)) &&
			p_visit(std::string("pitch_roll_axis"), &p_state.pitch_roll_axis, sizeof(p_state.pitch_roll_axis)) &&
			p_visit(std::string("pitch_roll_lambda"), &p_state.pitch_roll_lambda, sizeof(p_state.pitch_roll_lambda)) &&
			p_visit(std::string("current_step"), &p_state.current_step, sizeof(p_state.current_step));
}

}

uint64_t vehicle_state_hash(const VehicleState &p_state) {
	uint64_t h = kFnvOffset;
	visit_fields(p_state, [&](const std::string &, const void *p_data, size_t p_size) {
		fnv(h, p_data, p_size);
		return true;
	});
	return h;
}

std::string vehicle_state_first_difference(const VehicleState &p_a, const VehicleState &p_b) {
	if (p_a.wheel_count != p_b.wheel_count) {
		return "wheel_count";
	}

	const unsigned char *base_a = reinterpret_cast<const unsigned char *>(&p_a);
	const unsigned char *base_b = reinterpret_cast<const unsigned char *>(&p_b);
	std::string out;
	visit_fields(p_a, [&](const std::string &p_name, const void *p_data, size_t p_size) {
		const size_t offset = size_t(static_cast<const unsigned char *>(p_data) - base_a);
		if (std::memcmp(p_data, base_b + offset, p_size) != 0) {
			out = p_name;
			return false;
		}
		return true;
	});
	return out;
}

}
