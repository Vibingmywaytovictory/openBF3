#include "bot_driver.h"

#include "../tdm_bot_movement.h"
#include "../tdm_bot_shared.h"
#include "bf3/core/math.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/server_view.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bot_crew.h"
#include "server_vehicles.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace bf3::host {

namespace {

ai::Vec2 flat(const sim::Vec3 &p_v) {
	const float length = std::hypot(p_v[0], p_v[2]);
	return length > 0.0f ? ai::Vec2{ p_v[0] / length, p_v[2] / length } : ai::Vec2{};
}

struct Trace {
	float distance = 0.0f;
	bool clear = false;
};

Trace trace(sim::NavQuery &r_query, sim::NavPolyRef p_poly, const sim::Vec3 &p_origin, const sim::Vec3 &p_direction,
		float p_length) {
	Trace t;
	sim::Vec3 reached{};
	if (!r_query.raycast(p_poly, p_origin, core::vec::add(p_origin, core::vec::scale(p_direction, p_length)), &reached,
				&t.clear)) {
		return t;
	}
	const sim::Vec3 d = core::vec::sub(reached, p_origin);
	t.distance = core::vec::length(d);
	return t;
}

ai::CarSpace car_space(sim::NavQuery &r_query, sim::NavPolyRef p_poly, const sim::Vec3 &p_origin,
		const sim::Vec3 &p_ahead, const sim::Vec3 &p_right, float p_width, float p_length, float p_reach) {
	ai::CarSpace s;
	const sim::Vec3 front_left = core::vec::normalize(
			{ p_length * p_ahead[0] - p_right[0] * p_width, 0.0f, p_length * p_ahead[2] - p_right[2] * p_width });
	const sim::Vec3 front_right = core::vec::normalize(
			{ p_length * p_ahead[0] + p_right[0] * p_width, 0.0f, p_length * p_ahead[2] + p_right[2] * p_width });
	const auto back = [](const sim::Vec3 &p_v) { return sim::Vec3{ -p_v[0], -p_v[1], -p_v[2] }; };
	s.front = trace(r_query, p_poly, p_origin, p_ahead, p_reach).distance;
	for (const sim::Vec3 &d : { front_left, front_right }) {
		const Trace t = trace(r_query, p_poly, p_origin, d, p_reach);
		if (!t.clear && t.distance > 0.0f) {
			s.front = std::min(t.distance, s.front);
		}
	}
	s.back = trace(r_query, p_poly, p_origin, back(p_ahead), p_reach).distance;
	for (const sim::Vec3 &d : { front_left, front_right }) {
		const Trace t = trace(r_query, p_poly, p_origin, back(d), p_reach);
		if (!t.clear && t.distance > 0.0f) {
			s.back = std::min(t.distance, s.back);
		}
	}
	const Trace right = trace(r_query, p_poly, p_origin, p_right, p_reach);
	s.right = right.clear ? p_reach : right.distance;
	const Trace left = trace(r_query, p_poly, p_origin, back(p_right), p_reach);
	s.left = left.clear ? p_reach : left.distance;
	return s;
}

double seconds_at(sim::Tick p_tick) {
	return double(p_tick) * double(sim::kTickSeconds);
}

}

void BotDriver::new_life() {
	path_goal_.reset();
	corridor_.clear();
	requested_ = false;
	following_ = false;
	stuck_ = false;
	status_ = 0;
	progress_ = {};
	angles_ = {};
}

void BotDriver::release(float p_forward_speed, sim::Command &r_cmd) {
	r_cmd.move_right = 0;
	r_cmd.move_forward = sim::quantize_axis(ai::car_reset_throttle(p_forward_speed));
}

void BotDriver::request(
		TdmShared &r_shared, uint8_t p_slot, const sim::Vec3 &p_from, float p_radius, const sim::Vec3 &p_goal) {
	sim::NavQuery &query = *r_shared.nav->query;
	const sim::Vec3 extent{ 2.0f * p_radius, kNavProbeHeight, 2.0f * p_radius };
	sim::NavPolyRef start = 0, goal = 0;
	sim::Vec3 start_at{}, goal_at{};
	requested_ = query.nearest(p_from, extent, &start, &start_at) && start != 0 &&
			query.nearest(p_goal, extent, &goal, &goal_at) && goal != 0;
	following_ = false;
	corridor_.clear();
	if (requested_) {
		r_shared.nav->paths->request(kVehiclePathKey + p_slot, start, start_at, goal, goal_at);
	}
}

bool BotDriver::drive(TdmShared &r_shared, uint8_t p_slot, sim::Tick p_tick, const std::optional<sim::Vec3> &p_goal,
		float p_arrive_m, sim::Command &r_cmd) {
	const ServerVehicles *const vehicles = r_shared.host.view->server_vehicles();
	const std::optional<VehicleSeat> seat = vehicles != nullptr ? vehicles->seat_of(p_slot) : std::nullopt;
	if (!seat || seat->seat != 0 || r_shared.nav == nullptr || r_shared.nav->query == nullptr ||
			r_shared.nav->paths == nullptr) {
		stuck_ = false;
		following_ = false;
		return false;
	}
	const LevelVehicleKind &kind = vehicles->level()->kinds[vehicles->level()->spawners[seat->vehicle].kind];
	const sim::VehicleState state = vehicles->world()->state(sim::VehicleSlot(seat->vehicle));
	const sim::Vec3 position = state.body.com_position;
	const core::Transform hull = sim::vehicles::hull_transform(state.body.rotation, position);
	const ai::Vec2 forward = flat(hull.forward);
	const ai::Vec2 normal = flat({ -hull.right[0], -hull.right[1], -hull.right[2] });
	const sim::Vec3 &velocity = state.body.linear_velocity;
	const float forward_speed = core::vec::dot(velocity, hull.forward);
	const double now = seconds_at(p_tick);
	const float radius = core::vec::length(kind.box.half);
	if (!p_goal) {
		stuck_ = false;
		following_ = false;
		release(forward_speed, r_cmd);
		return true;
	}
	if (!path_goal_ || core::vec::horizontal_distance(*path_goal_, *p_goal) > tdm_bot::kGoalMoveMetres) {
		path_goal_ = p_goal;
		progress_.clear(now);
		status_ = 0;
		request(r_shared, p_slot, position, radius, *p_goal);
	}
	sim::NavQuery &query = *r_shared.nav->query;
	r_shared.nav->run_once(p_tick);
	if (requested_ && !following_) {
		const sim::NavPathStatus st = r_shared.nav->paths->take(kVehiclePathKey + p_slot, polys_);
		if (st == sim::NavPathStatus::Done || st == sim::NavPathStatus::Partial) {
			corridor_.reset(polys_, position, *p_goal);
			following_ = !corridor_.empty();
			requested_ = following_;
		} else if (st == sim::NavPathStatus::Failed) {
			requested_ = false;
		}
	}
	const float to_goal = core::vec::horizontal_distance(*p_goal, position);
	if (!following_ || to_goal <= p_arrive_m) {
		stuck_ = false;
		release(forward_speed, r_cmd);
		return true;
	}
	if (stuck_) {
		release(forward_speed, r_cmd);
		stuck_ = !progress_.check(now, position, sim::axis_value(r_cmd.move_forward), 0.0f);
		return true;
	}
	if (!corridor_.move_position(query, position)) {
		request(r_shared, p_slot, position, radius, *p_goal);
		release(forward_speed, r_cmd);
		return true;
	}
	std::array<sim::NavCorner, sim::kMaxNavCorners> corners{};
	const uint32_t count = corridor_.corners(query, corners.data(), uint32_t(corners.size()));
	if (count == 0) {
		release(forward_speed, r_cmd);
		return true;
	}
	std::array<sim::Vec3, sim::kMaxNavCorners> points{};
	for (uint32_t i = 0; i < count; ++i) {
		points[i] = corners[i].position;
	}
	const sim::Vec3 &target = points[0];
	const sim::Vec3 to_target{ target[0] - position[0], 0.0f, target[2] - position[2] };
	const float target_distance = std::hypot(to_target[0], to_target[2]);
	const float speed = core::vec::length(velocity);
	const float largest = angles_.largest(now, position, forward, speed, std::span(points.data(), count));
	const float turn_factor = ai::car_turn_factor(speed, largest);

	ai::CarSituation situation;
	situation.radius = radius;
	situation.max_speed = change_vehicle_max_speed(*kind.model);
	situation.forward = forward;
	situation.normal = normal;
	situation.desired = target_distance > 1e-06f
			? ai::Vec2{ to_target[0] / target_distance, to_target[2] / target_distance }
			: ai::Vec2{ to_target[0], to_target[2] };
	situation.goal_distance = to_goal;
	situation.forward_speed = forward_speed;
	sim::NavPolyRef poly = 0;
	sim::Vec3 origin{};
	if (query.nearest(position, { 2.0f * radius, kNavProbeHeight, 2.0f * radius }, &poly, &origin) && poly != 0) {
		const sim::Vec3 ahead{ forward.x, 0.0f, forward.y };
		const sim::Vec3 right{ normal.x, 0.0f, normal.y };
		situation.space = car_space(query, poly, origin, ahead, right, 2.0f * kind.box.half[0], 2.0f * kind.box.half[2],
				ai::car_trace_length(radius));
	}
	const ai::CarCommand command = ai::car_action_status(status_, situation);
	if (command.mode == ai::kCarArrivedMode) {
		release(forward_speed, r_cmd);
		return true;
	}
	const float smallest = std::min(kind.box.half[0], kind.box.half[2]);
	const float approach = ai::car_approach_factor(target_distance, smallest);
	const sim::Vec3 rotational{ core::vec::dot(state.body.angular_velocity, hull.right),
		core::vec::dot(state.body.angular_velocity, hull.up),
		core::vec::dot(state.body.angular_velocity, hull.forward) };
	const ai::CarControls controls = ai::car_control_towards_direction(ai::Vec2{ velocity[0], velocity[2] }, rotational,
			std::min(turn_factor, approach) * situation.max_speed, command);
	stuck_ = !progress_.check(now, position, controls.throttle, controls.turn);
	r_cmd.move_forward = sim::quantize_axis(controls.throttle);
	r_cmd.move_right = sim::quantize_axis(controls.turn);
	return true;
}

}
