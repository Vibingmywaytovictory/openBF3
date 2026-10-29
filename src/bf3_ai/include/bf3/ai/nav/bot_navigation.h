#pragma once

#include "bf3/ai/nav/nav_clusters.h"
#include "bf3/sim/bots/walker_brain.h"
#include "bf3/sim/nav_query.h"

#include <cstdint>
#include <vector>

namespace bf3::ai {

struct BotNavigationParams {
	float arrive_distance_m = 2.0f;
	float point_distance_m = 2.0f;
	uint32_t clusters_ahead = 4;
	sim::Tick progress_ticks = 180;
	float progress_m = 1.0f;
	float off_path_m = 3.0f;
	Vec3 search_extents{ 2.0f, 4.0f, 2.0f };
};

enum class BotNavigationState : uint8_t { Idle, ClusterSearch, PolygonSearch, Following, Arrived, Failed };

struct BotNavigationWorld {
	const sim::NavWorld *nav = nullptr;
	const NavClusterMap *clusters = nullptr;
	const MaterialManager *materials = nullptr;
	sim::NavQuery *query = nullptr;
	sim::bots::PathService *paths = nullptr;
};

class BotNavigation {
public:
	BotNavigation(uint32_t p_slot, const BotNavigationParams &p_params = {});

	bool init_pathfinding(const BotNavigationWorld &p_world, const Vec3 &p_from, const Vec3 &p_to, sim::Tick p_now);
	void update(const BotNavigationWorld &p_world, const Vec3 &p_position, sim::Tick p_now);

	BotNavigationState state() const { return state_; }
	bool is_path_search_pending() const { return state_ == BotNavigationState::PolygonSearch; }
	bool is_running() const {
		return state_ == BotNavigationState::PolygonSearch || state_ == BotNavigationState::Following;
	}
	bool is_stuck() const { return stuck_; }
	bool check_arrived_at_point(const Vec3 &p_position, float p_distance) const;
	bool still_on_path(const Vec3 &p_position) const;
	const Vec3 &steer_point() const { return steer_; }
	const Vec3 &goal() const { return goal_; }
	const std::vector<uint32_t> &cluster_path() const { return cluster_path_; }
	uint32_t cluster_cursor() const { return cluster_cursor_; }
	uint32_t searches() const { return searches_; }

	void set_is_stuck(bool p_stuck) { stuck_ = p_stuck; }
	void clear_progress(const Vec3 &p_position, sim::Tick p_now);
	void check_progress(const Vec3 &p_position, sim::Tick p_now);
	void reset();
	void reset_all();

private:
	bool request_polygon_search(const BotNavigationWorld &p_world, const Vec3 &p_from);
	Vec3 search_target() const;

	uint32_t slot_ = 0;
	BotNavigationParams params_;
	BotNavigationState state_ = BotNavigationState::Idle;
	Vec3 goal_{};
	sim::NavPolyRef goal_poly_ = 0;
	std::vector<uint32_t> cluster_path_;
	std::vector<Vec3> cluster_points_;
	uint32_t cluster_cursor_ = 0;
	uint32_t search_cursor_ = 0;
	sim::NavCorridor corridor_;
	std::vector<sim::NavPolyRef> polys_;
	ClusterNodeManager nodes_;
	Vec3 steer_{};
	Vec3 progress_position_{};
	sim::Tick progress_tick_ = 0;
	bool stuck_ = false;
	uint32_t searches_ = 0;
};

}
