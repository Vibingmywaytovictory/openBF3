#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::level {
struct LevelSpawns;
}

namespace bf3::sim {

class CollisionWorld;
class SoldierMotor;

struct SpawnPoint {
	Vec3 position{};
	uint16_t yaw = 0;
	int team = 0;
	float priority = 1.0f;
	uint32_t index = 0;
};

struct SettleOptions {
	float probe_up = 1.0f;
	float probe_down = 2.0f;
	float max_lift = 0.6f;
	float ring_step = 0.1f;
	uint32_t rings = 5;
	uint32_t ring_samples = 8;
	uint32_t settle_ticks = 60;
	float hover_tolerance = 0.01f;
};

struct SettleResult {
	bool ok = false;
	std::string failure;
	Vec3 placed{};

	SoldierState state;
	float floor_y = 0;
	float lift = 0;
	float shift = 0;

	float gap = 0;
	bool overlapping = false;
};

std::vector<SpawnPoint> to_spawn_points(const core::level::LevelSpawns &p_spawns);

SettleResult place_spawn(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, uint32_t p_life, Tick p_tick,
		const SettleOptions &p_options = {});
void judge_settle(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, const SettleOptions &p_options,
		SettleResult &r_result);

void stamp_spawn(const SoldierMotor &p_motor, Tick p_tick, SoldierState &r_state);

SettleResult settle_spawn(const SoldierMotor &p_motor, const CollisionWorld &p_world, const SpawnPoint &p_point, uint32_t p_life, Tick p_tick,
		const SettleOptions &p_options = {});

size_t choose_spawn(const std::vector<SpawnPoint> &p_points, int p_team, const std::vector<Vec3> &p_live_soldiers, float p_protection_radius,
		uint64_t p_seed);

std::optional<uint16_t> find_clear_heading(const SoldierMotor &p_motor, const CollisionWorld &p_world, const Vec3 &p_feet, float p_length,
		float p_max_rise = 0.05f, uint32_t p_headings = 64, uint16_t p_first_yaw = 0);

std::optional<Vec3> find_drop_spot(const SoldierMotor &p_motor, const CollisionWorld &p_world, float p_clearance = 50.0f, float p_grid = 5.0f);

struct FriendlyDeath {
	Vec3 position{};
	uint32_t tick = 0;
	uint8_t team = 0;
};
struct SpawnQuery {
	std::span<const SpawnPoint> points;
	uint8_t team = 0;
	std::span<const Vec3> enemies;
	std::span<const Vec3> enemy_eyes;
	std::span<const FriendlyDeath> recent_deaths;
	float protection_radius = 10.0f;
	float friendly_block_radius = 30.0f;
	int friendly_block_count = 3;
	uint32_t friendly_block_ticks = 1800;
	uint32_t tick = 0;
	uint64_t seed = 0;
	int max_los_rays = 16;
	const CollisionWorld *world = nullptr;

	float safe_distance = 0.0f;
	float chest_height = 1.3f;
};

size_t choose_spawn_scored(const SpawnQuery &p_query);

}
