#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/sim/sim_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::host {

enum class FirefightMode : uint8_t { Pairs = 0, Free = 1 };

struct FirefightObserver {
	bool valid = false;
	sim::Vec3 position{};
	float yaw = 0.0f;
};

struct FirefightStats {
	std::atomic<uint32_t> bots{ 0 };
	std::atomic<uint32_t> lanes{ 0 };
	std::atomic<uint32_t> lanes_rejected{ 0 };
	std::atomic<uint64_t> bursts{ 0 };
	std::atomic<uint64_t> reloads{ 0 };
	std::atomic<uint64_t> retargets{ 0 };
	std::atomic<uint64_t> no_target_fire{ 0 };
	std::atomic<uint64_t> placements{ 0 };

	std::atomic<uint64_t> los_rays{ 0 };
	std::atomic<uint64_t> hunts{ 0 };
	std::atomic<uint64_t> strafes{ 0 };
	std::atomic<uint64_t> mode_switches{ 0 };
	std::atomic<uint64_t> sidearm_switches{ 0 };
	std::atomic<uint32_t> walk_unavailable{ 0 };
	std::atomic<uint32_t> lanes_shared{ 0 };
};

struct FirefightConfig {
	FirefightMode mode = FirefightMode::Pairs;
	uint32_t count = 64;
	FirefightObserver observer;

	float aim_sigma_start_deg = 1.5f;
	float aim_sigma_end_deg = 0.5f;
	float aim_sigma_decay_s = 1.0f;

	uint32_t burst_min_rounds = 3;
	uint32_t burst_max_rounds = 6;
	float burst_pause_min_s = 0.3f;
	float burst_pause_max_s = 0.6f;
	float semi_rate_hz = 2.0f;
	float reload_fraction = 0.5f;

	float lane_fan_deg = 35.0f;
	float lane_min_m = 15.0f;
	float lane_max_m = 60.0f;
	float pair_min_m = 20.0f;
	float pair_max_m = 40.0f;
	uint32_t lane_retries = 8;

	uint32_t retarget_ticks = 30;
	float no_target_s = 2.0f;
	bool walk = true;

	std::shared_ptr<FirefightStats> stats;
};

constexpr uint64_t firefight_seed(uint64_t p_match_seed, uint8_t p_slot) {
	return p_match_seed ^ uint64_t(p_slot);
}

struct FirefightLane {
	sim::Vec3 a{}, b{};
	float yaw_a = 0.0f, yaw_b = 0.0f;
	uint32_t tries = 0;
};

core::Result<std::vector<FirefightLane>> firefight_layout(const sim::CollisionWorld &p_world, const FirefightConfig &p_config, uint64_t p_seed);
struct BotNav;
core::Result<std::vector<FirefightLane>> firefight_layout(const sim::CollisionWorld &p_world, const FirefightConfig &p_config, uint64_t p_seed,
		BotNav *p_nav, uint32_t *r_rejected = nullptr);

struct FirefightShared;

class FirefightBrain final : public CommandSource {
public:
	FirefightBrain(GameServer &r_server, std::shared_ptr<const FirefightConfig> p_config, uint32_t p_index, const FirefightLane *p_lane);

	FirefightBrain(std::shared_ptr<FirefightShared> p_shared, uint32_t p_index, const FirefightLane *p_lane, uint8_t p_end, float p_offset);
	~FirefightBrain() override;
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override;
	void set_slot(uint8_t p_slot);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

core::Status add_firefight_bots(GameServer &r_server, const GameServerConfig &p_config);

}
