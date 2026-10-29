#include "bf3/ai/nav/bot_navigation.h"

#include "bf3/ai/squad/squad_world.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace bf3::ai {

namespace {

float horizontal_sq(const Vec3 &p_a, const Vec3 &p_b) {
	const float dx = p_a.x - p_b.x;
	const float dz = p_a.z - p_b.z;
	return dx * dx + dz * dz;
}

const ClusterPortal *portal_between(const ClusterGraph &p_graph, uint32_t p_from, uint32_t p_to) {
	for (uint32_t i = 0; i < p_graph.cluster(p_from).number_of_portals; ++i) {
		const ClusterPortal &portal = p_graph.cluster_portal(p_from, i);
		if (portal.to_cluster == p_to) {
			return &portal;
		}
	}
	return nullptr;
}

}

BotNavigation::BotNavigation(uint32_t p_slot, const BotNavigationParams &p_params) : slot_(p_slot), params_(p_params) {}

bool BotNavigation::init_pathfinding(
		const BotNavigationWorld &p_world, const Vec3 &p_from, const Vec3 &p_to, sim::Tick p_now) {
	reset();
	goal_ = p_to;
	steer_ = p_to;
	clear_progress(p_from, p_now);
	if (p_world.nav == nullptr || p_world.clusters == nullptr || p_world.materials == nullptr ||
			p_world.query == nullptr || p_world.paths == nullptr) {
		state_ = BotNavigationState::Failed;
		return false;
	}
	sim::NavPolyRef start = 0;
	Vec3 start_point{};
	Vec3 goal_point{};
	if (!p_world.query->nearest(p_from, params_.search_extents, &start, &start_point) ||
			!p_world.query->nearest(p_to, params_.search_extents, &goal_poly_, &goal_point)) {
		state_ = BotNavigationState::Failed;
		return false;
	}
	goal_ = goal_point;
	state_ = BotNavigationState::ClusterSearch;
	const uint32_t from = p_world.clusters->cluster_of(*p_world.nav, start);
	const uint32_t to = p_world.clusters->cluster_of(*p_world.nav, goal_poly_);
	if (from == kNoCluster || to == kNoCluster ||
			p_world.clusters->graph.shortest_path(from, to, *p_world.materials, nodes_, cluster_path_) < 0.0f) {
		cluster_path_.clear();
		state_ = BotNavigationState::Failed;
		return false;
	}
	cluster_points_.clear();
	for (size_t i = 0; i + 1 < cluster_path_.size(); ++i) {
		const ClusterPortal *portal = portal_between(p_world.clusters->graph, cluster_path_[i], cluster_path_[i + 1]);
		if (portal == nullptr) {
			state_ = BotNavigationState::Failed;
			return false;
		}
		cluster_points_.push_back(portal->position);
	}
	cluster_points_.push_back(goal_);
	cluster_cursor_ = 0;
	return request_polygon_search(p_world, p_from);
}

Vec3 BotNavigation::search_target() const {
	return cluster_points_[search_cursor_];
}

bool BotNavigation::request_polygon_search(const BotNavigationWorld &p_world, const Vec3 &p_from) {
	const uint32_t last = uint32_t(cluster_points_.size()) - 1;
	search_cursor_ = std::min(last, cluster_cursor_ + std::max<uint32_t>(params_.clusters_ahead, 1) - 1);
	sim::NavPolyRef start = 0;
	Vec3 start_point{};
	sim::NavPolyRef target = 0;
	Vec3 target_point{};
	if (!p_world.query->nearest(p_from, params_.search_extents, &start, &start_point)) {
		state_ = BotNavigationState::Failed;
		return false;
	}
	if (search_cursor_ == last) {
		target = goal_poly_;
		target_point = goal_;
	} else if (!p_world.query->nearest(search_target(), params_.search_extents, &target, &target_point)) {
		state_ = BotNavigationState::Failed;
		return false;
	}
	cluster_points_[search_cursor_] = target_point;
	p_world.paths->request(slot_, start, start_point, target, target_point);
	state_ = BotNavigationState::PolygonSearch;
	++searches_;
	return true;
}

void BotNavigation::update(const BotNavigationWorld &p_world, const Vec3 &p_position, sim::Tick p_now) {
	if (state_ == BotNavigationState::PolygonSearch) {
		const sim::NavPathStatus status = p_world.paths->take(slot_, polys_);
		if (status == sim::NavPathStatus::Idle || status == sim::NavPathStatus::InProgress) {
			return;
		}
		if (status == sim::NavPathStatus::Failed || polys_.empty()) {
			state_ = BotNavigationState::Failed;
			return;
		}
		corridor_.reset(polys_, p_position, search_target());
		state_ = BotNavigationState::Following;
	}
	if (state_ != BotNavigationState::Following) {
		return;
	}
	corridor_.move_position(*p_world.query, p_position);
	std::array<sim::NavCorner, 2> corners{};
	const uint32_t count = corridor_.corners(*p_world.query, corners.data(), uint32_t(corners.size()));
	const float point_sq = params_.point_distance_m * params_.point_distance_m;
	steer_ = search_target();
	if (count > 0) {
		steer_ = corners[0].position;
		if (count > 1 && horizontal_sq(p_position, steer_) < point_sq) {
			steer_ = corners[1].position;
		}
	}
	const bool at_target = horizontal_sq(p_position, search_target()) < point_sq;
	if (at_target || (count > 0 && corners[0].end && horizontal_sq(p_position, corners[0].position) < point_sq)) {
		if (search_cursor_ + 1 >= cluster_points_.size()) {
			if (check_arrived_at_point(p_position, params_.arrive_distance_m)) {
				state_ = BotNavigationState::Arrived;
				return;
			}
		} else {
			cluster_cursor_ = search_cursor_ + 1;
			request_polygon_search(p_world, p_position);
			return;
		}
	}
	check_progress(p_position, p_now);
}

bool BotNavigation::check_arrived_at_point(const Vec3 &p_position, float p_distance) const {
	return horizontal_sq(p_position, goal_) < p_distance * p_distance &&
			std::fabs(p_position.y - goal_.y) < 2.0f * p_distance;
}

bool BotNavigation::still_on_path(const Vec3 &p_position) const {
	if (state_ != BotNavigationState::Following || corridor_.empty()) {
		return state_ == BotNavigationState::PolygonSearch;
	}
	return horizontal_sq(p_position, corridor_.position()) <= params_.off_path_m * params_.off_path_m;
}

void BotNavigation::clear_progress(const Vec3 &p_position, sim::Tick p_now) {
	progress_position_ = p_position;
	progress_tick_ = p_now;
	stuck_ = false;
}

void BotNavigation::check_progress(const Vec3 &p_position, sim::Tick p_now) {
	if (distance_sq(p_position, progress_position_) >= params_.progress_m * params_.progress_m) {
		clear_progress(p_position, p_now);
		return;
	}
	if (p_now - progress_tick_ >= params_.progress_ticks) {
		stuck_ = true;
	}
}

void BotNavigation::reset() {
	state_ = BotNavigationState::Idle;
	cluster_path_.clear();
	cluster_points_.clear();
	cluster_cursor_ = 0;
	search_cursor_ = 0;
	polys_.clear();
	corridor_.clear();
	goal_poly_ = 0;
}

void BotNavigation::reset_all() {
	reset();
	stuck_ = false;
	searches_ = 0;
}

}
