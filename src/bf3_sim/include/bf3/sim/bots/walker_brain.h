#pragma once

#include "bf3/sim/bots/bot_stats.h"
#include "bf3/sim/command.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::sim::bots {

struct BotGoal {
	enum class Kind : uint8_t { NavPoint = 0, Flag = 1, Hq = 2, Mcom = 3, Spawn = 4 };
	Kind kind = Kind::NavPoint;
	Vec3 position{};
	NavPolyRef poly = 0;
	float arrive_radius = 1.5f;
	uint32_t index = 0;
};

struct WalkerParams {
	float think_hz = 10.0f;
	float objective_share = 0.5f;
	float sprint_share = 0.4f;
	float crouch_share = 0.15f;
	float yaw_rate_deg_s = 540.0f;

	float corner_lookahead = 0.4f;
	float stuck_seconds = 2.0f;
	float stuck_progress = 0.5f;
	uint32_t max_repaths = 3;
	float off_mesh_radius = 2.0f;
	float goal_min_distance = 20.0f;

	bool operator==(const WalkerParams &) const = default;
};

class PathService {
public:
	struct Options {
		uint32_t iterations_per_tick = 4096;
	};
	PathService(NavQuery &p_query, const Options &p_options);
	~PathService();
	PathService(const PathService &) = delete;
	PathService &operator=(const PathService &) = delete;

	void request(uint32_t p_slot, NavPolyRef p_start, const Vec3 &p_start_point, NavPolyRef p_goal, const Vec3 &p_goal_point);

	void run();

	NavPathStatus take(uint32_t p_slot, std::vector<NavPolyRef> &r_polys);
	size_t pending() const;
	const PathServiceStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct WalkerView {
	Tick tick = 0;
	uint32_t slot = 0;
	const SoldierState *self = nullptr;
	float stand_eye_height = 1.6f;
	std::span<const BotGoal> objectives;
	const NavComponents *components = nullptr;
	uint32_t main_component = 0;
	NavQuery *query = nullptr;
	PathService *paths = nullptr;
};

class WalkerBrain {
public:
	WalkerBrain(uint32_t p_slot, uint64_t p_seed, const WalkerParams &p_params);
	~WalkerBrain();
	WalkerBrain(WalkerBrain &&) noexcept;
	WalkerBrain &operator=(WalkerBrain &&) noexcept;

	Command command(const WalkerView &p_view);
	const WalkerStats &stats() const;

	const BotGoal *goal() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
