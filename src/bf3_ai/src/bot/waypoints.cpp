#include "bf3/ai/bot/waypoints.h"

#include "bf3/ai/knowledge/information.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/squad/squad_world.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

namespace {

float distance_xz_sq(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a.x - p_b.x;
	const float dz = p_a.z - p_b.z;
	return dx * dx + dz * dz;
}

Vec3 close_to_position(const Information &p_info) {
	const IPIPhysical *const physical = p_info.physical();
	if (physical == nullptr) {
		return p_info.position();
	}
	const Vec3 offset = p_info.transformation().transform_vector(physical->sphere_local_offset());
	const Vec3 &position = p_info.position();
	return { position.x + offset.x, position.y + offset.y, position.z + offset.z };
}

}

float WPEmpty::urgency(const Vec3 &p_position, const WaypointBot &) {
	changed_ = true;
	position_ = p_position;
	return 0.0f;
}

bool WPEmpty::goal_point(Vec3 &r_out) const {
	r_out = position_;
	return true;
}

WPMoveTo::WPMoveTo(float p_radius, float p_step_radius, const Goals &p_goals, const WaypointAreas &p_areas) :
		radius_(p_radius), step_radius_(std::max(p_step_radius, 1.0f)), goals_(p_goals), areas_(p_areas) {}

WPMoveTo::WPMoveTo(float p_radius, float p_step_radius, const std::vector<Vec3> &p_positions, int p_last_area,
		const WaypointAreas &p_areas) :
		radius_(p_radius), step_radius_(std::max(p_step_radius, 2.0f)), areas_(p_areas) {
	for (const Vec3 &p : p_positions) {
		goals_.emplace_back(p, -1);
	}
	if (!goals_.empty()) {
		goals_.back().second = p_last_area;
	}
}

float WPMoveTo::urgency(const Vec3 &p_position, const WaypointBot &p_bot) {
	if (goals_.empty()) {
		finished_ = true;
		return 0.0f;
	}
	const int area = goals_.front().second;
	float d_sq = distance_sq(goals_.front().first, p_position);
	if (goals_.size() == 1) {
		const float radius = std::max(std::ceil(radius_), p_bot.smallest_radius);
		const float radius_sq = radius * radius;
		if (!(radius_sq <= d_sq)) {
			finished_ = true;
			error_ = false;
			return 0.0f;
		}
		float factor = 1.0f;
		if (!areas_.exists(area)) {
			factor = error_ ? 0.0f : 1.0f;
		} else {
			if (areas_.owned(area, p_bot.side)) {
				if (!areas_.inside(area, p_position)) {
					const float side_radius = areas_.size(area, p_bot.side);
					d_sq -= side_radius * side_radius;
				} else {
					factor = 0.0f;
					d_sq = 0.0f;
				}
			} else {
				factor = 2.0f;
			}
			if (error_) {
				areas_.randomize_position(area, p_bot.side, 0.9f, goals_.front().first);
				error_ = false;
			}
		}
		const float scaled = radius_sq * 1.5f;
		finished_ = !(scaled <= d_sq);
		return std::clamp(d_sq / scaled, 0.1f, 1.0f) * factor;
	}
	float radius = std::max(std::ceil(step_radius_), p_bot.smallest_radius);
	if (area == -1) {
		radius = std::max(p_bot.smallest_radius, 10.0f);
	}
	if (radius * radius <= d_sq) {
		if (!areas_.exists(area)) {
			if (!error_) {
				return 1.0f;
			}
		} else if (!areas_.inside(area, p_position)) {
			if (error_) {
				areas_.randomize_position(area, p_bot.side, 0.9f, goals_.front().first);
				error_ = false;
			}
			return 1.0f;
		}
	}
	goals_.pop_front();
	changed_ = true;
	error_ = false;
	return 1.0f;
}

bool WPMoveTo::goal_point(Vec3 &r_out) const {
	if (goals_.empty()) {
		return false;
	}
	r_out = goals_.front().first;
	return true;
}

void WPMoveTo::coords(std::vector<Vec3> &r_out) const {
	for (const auto &[position, area] : goals_) {
		r_out.push_back(position);
	}
}

WPCloseTo::WPCloseTo(uint32_t p_target, float p_radius, const InformationManager &p_information) :
		target_(p_target), radius_(std::max(p_radius, 5.0f)), information_(p_information) {
	const Information *const info = information_.information(target_);
	if (info == nullptr) {
		return;
	}
	if (const IPIPhysical *const physical = info->physical()) {
		radius_ = std::max(radius_, physical->smallest_radius() + physical->smallest_radius());
	}
	goal_ = close_to_position(*info);
}

float WPCloseTo::urgency(const Vec3 &p_position, const WaypointBot &) {
	if (error_) {
		return 0.0f;
	}
	const Information *const info = information_.information(target_);
	if (info == nullptr) {
		finished_ = true;
		return 0.0f;
	}
	const Vec3 point = close_to_position(*info);
	distance_sq_ = distance_sq(point, p_position);
	const float radius_sq = radius_ * radius_;
	const float u = std::min(distance_sq_ / (radius_sq * 4.0f), 1.0f);
	if (radius_sq <= distance_sq_) {
		if (radius_sq < distance_sq(goal_, point)) {
			goal_ = point;
			changed_ = true;
		}
		return u;
	}
	changed_ = true;
	return 0.0f;
}

bool WPCloseTo::goal_point(Vec3 &r_out) const {
	if (information_.information(target_) == nullptr) {
		return false;
	}
	r_out = goal_;
	return true;
}

float WPCloseTo::modify_max_speed(float p_speed) const {
	const Information *const info = information_.information(target_);
	if (info == nullptr) {
		return p_speed;
	}
	const IPIMobile *const mobile = info->mobile();
	if (mobile == nullptr) {
		return radius_ < distance_sq_ ? p_speed : 0.0f;
	}
	float limit = 0.0f;
	if (radius_ >= distance_sq_) {
		limit = mobile->speed_magnitude();
	} else {
		if (radius_ < distance_sq_ + distance_sq_) {
			return p_speed;
		}
		limit = mobile->speed_magnitude() * 1.33f * distance_sq_ / radius_;
	}
	return std::min(limit, p_speed);
}

WPPatrol::WPPatrol(float p_radius, const std::vector<Vec3> &p_points) :
		radius_(std::max(p_radius, 5.0f)), points_(p_points.begin(), p_points.end()) {}

float WPPatrol::urgency(const Vec3 &p_position, const WaypointBot &) {
	if (error_ || points_.empty()) {
		return 0.0f;
	}
	if (radius_ * radius_ > distance_xz_sq(p_position, points_.front())) {
		points_.push_back(points_.front());
		points_.pop_front();
		changed_ = true;
	}
	return 1.0f;
}

bool WPPatrol::goal_point(Vec3 &r_out) const {
	if (points_.empty()) {
		return false;
	}
	r_out = points_.front();
	return true;
}

void WPPatrol::coords(std::vector<Vec3> &r_out) const {
	r_out.assign(points_.begin(), points_.end());
}

WPAltitudeCloseTo::WPAltitudeCloseTo(
		uint32_t p_target, float p_radius, float p_ceiling, float p_distance, const InformationManager &p_information) :
		target_(p_target), radius_(p_radius), ceiling_(p_ceiling), distance_(p_distance), information_(p_information) {}

float WPAltitudeCloseTo::urgency(const Vec3 &p_position, const WaypointBot &) {
	if (error_) {
		return 0.0f;
	}
	const Information *const info = information_.information(target_);
	if (info == nullptr) {
		finished_ = true;
		return 0.0f;
	}
	const float dx = info->position().x - p_position.x;
	const float dz = info->position().z - p_position.z;
	if (radius_ * radius_ > dx * dx * (dz * dz) && ceiling_ > p_position.y) {
		changed_ = true;
		return 0.0f;
	}
	return 1.0f;
}

bool WPAltitudeCloseTo::goal_point(Vec3 &r_out) const {
	const Information *const info = information_.information(target_);
	if (info == nullptr) {
		return false;
	}
	r_out = info->position();
	return true;
}

WPAltitudeMoveTo::WPAltitudeMoveTo(
		float p_radius, float p_height, float p_distance, const std::vector<Vec3> &p_positions) :
		radius_(p_radius), height_(p_height), distance_(p_distance),
		positions_(p_positions.begin(), p_positions.end()) {}

float WPAltitudeMoveTo::urgency(const Vec3 &p_position, const WaypointBot &p_bot) {
	if (error_) {
		return 0.0f;
	}
	if (positions_.empty()) {
		finished_ = true;
		return 0.0f;
	}
	const float radius_sq = radius_ * radius_;
	const float height_sq = height_ * height_;
	const float d_sq = distance_xz_sq(p_position, positions_.front());
	const float dy = p_position.y - positions_.front().y;
	const float dy_sq = dy * dy;
	if (positions_.size() == 1) {
		if (2500.0f > d_sq && p_bot.air_born) {
			return 0.0f;
		}
		if (radius_sq > d_sq && height_sq > dy_sq) {
			return (d_sq + dy_sq) / (radius_sq + height_sq);
		}
		return 1.0f;
	}
	if ((radius_sq > d_sq && height_sq > dy_sq) || d_sq > last_distance_sq_) {
		positions_.pop_front();
		changed_ = true;
	}
	last_distance_sq_ = d_sq;
	if (radius_sq > d_sq && dy_sq > height_sq) {
		return (d_sq + dy_sq) / (radius_sq + height_sq);
	}
	return 1.0f;
}

bool WPAltitudeMoveTo::goal_point(Vec3 &r_out) const {
	if (positions_.empty()) {
		return false;
	}
	r_out = positions_.front();
	return true;
}

void WPAltitudeMoveTo::coords(std::vector<Vec3> &r_out) const {
	r_out.assign(positions_.begin(), positions_.end());
}

WPAltitudePatrol::WPAltitudePatrol(
		float p_radius, float p_height, float p_distance, const std::vector<Vec3> &p_positions) :
		radius_(p_radius), height_(p_height), distance_(p_distance),
		positions_(p_positions.begin(), p_positions.end()) {}

float WPAltitudePatrol::urgency(const Vec3 &p_position, const WaypointBot &) {
	if (error_ || positions_.empty()) {
		return 0.0f;
	}
	if (radius_ * radius_ > distance_xz_sq(p_position, positions_.front()) &&
			height_ > std::fabs(p_position.y - positions_.front().y)) {
		positions_.push_back(positions_.front());
		positions_.pop_front();
		changed_ = true;
	}
	return 1.0f;
}

bool WPAltitudePatrol::goal_point(Vec3 &r_out) const {
	if (positions_.empty()) {
		return false;
	}
	r_out = positions_.front();
	return true;
}

void WPAltitudePatrol::coords(std::vector<Vec3> &r_out) const {
	r_out.assign(positions_.begin(), positions_.end());
}

}
