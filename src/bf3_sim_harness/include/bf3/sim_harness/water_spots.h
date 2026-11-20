#pragma once

#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/water.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace bf3::sim {

inline constexpr float kWaterSpotDepth = 2.5f;
inline constexpr float kWaterSpotGrid = 8.0f;
inline constexpr float kWaterSpotBankReach = 40.0f;
inline constexpr float kWaterSpotBankStep = 0.5f;
inline constexpr float kWaterSpotBankRise = 0.2f;
inline constexpr size_t kWaterSpotCandidates = 32;

struct WaterSpot {
	Vec3 bank{};
	Vec3 deep{};
	uint16_t yaw = 0;
	float surface = 0.0f;
	float run = 0.0f;
};

std::optional<WaterSpot> find_water_spot(const SoldierMotor &p_motor, const CollisionWorld &p_world,
		const WaterSurface &p_water, std::span<const SpawnPoint> p_points);

float water_spot_along(const WaterSpot &p_spot, const Vec3 &p_at);

std::optional<float> bed_below(const CollisionWorld &p_world, float p_x, float p_z, float p_from);

struct WaterSpotRun {
	SoldierState in;
	SoldierState out;
	float surface = 0.0f;
	float bed = 0.0f;
	uint32_t swim_ticks = 0;
	uint32_t first_swim_tick = 0;
	bool reached = false;
	bool left = false;
};

WaterSpotRun run_at_water_spot(const SoldierMotor &p_motor, const CollisionWorld &p_world, const WaterSurface &p_water,
		const WaterSpot &p_spot);

}
