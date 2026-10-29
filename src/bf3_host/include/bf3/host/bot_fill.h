#pragma once

#include "bf3/ai/nav/nav_clusters.h"
#include "bf3/core/result.h"
#include "bf3/host/bot_fill_stats.h"
#include "bf3/sim/bots/walker_brain.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/spawn.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::core {
class Cache;
}
namespace bf3::core::level {
struct LevelCombatTiles;
struct ModeObjectiveDef;
struct CombatAreaDef;
}
namespace bf3::sim {
class CollisionWorld;
class SoldierMotor;
}

namespace bf3::host {

struct LevelDatabase;
struct SharedLevel;
struct LoadProgress;

struct BotPointRow {
	enum class Kind : uint8_t { Flag = 0, Hq = 1, Mcom = 2, Spawn = 3 };
	Kind kind = Kind::Spawn;
	uint32_t index = 0;
	sim::Vec3 point{};
	sim::NavPointCheck check;
	bool on_mesh = false;
	bool in_main = false;

	bool in_main_weak = false;

	bool reaches_main = false;
	bool settled = false;
};

struct BotSpawns {
	std::vector<sim::SpawnPoint> points;
	uint32_t level_points = 0;
	uint32_t in_main = 0;
	uint32_t excluded = 0;
	bool teams_merged = false;
	uint32_t synthetic = 0;
};

struct BotStrategicArea {
	enum class Kind : uint8_t { Flag = 0, Hq = 1, Mcom = 2, TdmCluster = 3 };
	Kind kind = Kind::TdmCluster;
	std::string name;
	sim::Vec3 centre{};
	sim::NavPolyRef poly = 0;
	float radius = 0.0f;
	int team = 0;
	bool takeable = false;
	uint32_t objective = UINT32_MAX;
	uint32_t cluster = UINT32_MAX;
	std::vector<uint32_t> spawns;
};

struct BotLevel {
	std::string level;
	std::string mode;
	std::shared_ptr<const sim::NavWorld> nav;
	sim::NavComponents components;

	uint32_t main_component = 0;
	std::vector<sim::bots::BotGoal> objectives;
	std::vector<BotPointRow> points;
	BotSpawns spawns;
	std::vector<sim::NavTileKey> region;
	std::shared_ptr<const ai::NavClusterMap> clusters;
	std::vector<BotStrategicArea> areas;
	double seconds_areas = 0.0;
	double seconds_nav = 0.0;
	double seconds_components = 0.0;
	double seconds_strategic = 0.0;
};

struct BotLoadOptions {
	core::Cache *cache = nullptr;
	unsigned threads = 0;

	sim::NavParams params;
	bool params_from_motor = true;
	uint32_t layer_mask = sim::query_mask::kMovement;

	uint32_t synthetic_spawns = 64;
	uint64_t seed = 1;

	const core::level::LevelCombatTiles *combat = nullptr;
};

core::Result<std::shared_ptr<const BotLevel>> load_bot_level(const LevelDatabase &p_db, const SharedLevel &p_shared, const BotLoadOptions &p_options,
		LoadProgress *p_progress);

float bot_point_radius(BotPointRow::Kind p_kind);
float bot_point_vertical(BotPointRow::Kind p_kind);
struct BotRowsResult {
	std::vector<BotPointRow> points;
	std::vector<sim::bots::BotGoal> objectives;
	uint32_t main = 0;
};

BotRowsResult bot_point_rows(const sim::NavWorld &p_nav, const sim::NavComponents &p_components, uint32_t p_main,
		std::span<const core::level::ModeObjectiveDef> p_objectives, std::span<const sim::SpawnPoint> p_spawns, const sim::SoldierMotor &p_motor,
		const sim::CollisionWorld &p_world, bool p_main_from_rows = false);

BotSpawns make_bot_spawns(std::span<const BotPointRow> p_rows, std::span<const sim::SpawnPoint> p_spawns, const sim::NavComponents &p_components,
		uint32_t p_main, sim::NavQuery &p_query, const sim::SoldierMotor &p_motor, const sim::CollisionWorld &p_world, uint32_t p_synthetic, uint64_t p_seed);

uint32_t main_weak_component(const sim::NavComponents &p_components, uint32_t p_main);

uint32_t mode_main_component(const sim::NavWorld &p_nav, const sim::NavComponents &p_components, std::span<const core::level::CombatAreaDef> p_areas);

struct BotFillConfig {
	std::shared_ptr<const BotLevel> level;
	uint32_t count = 0;

	bool spawn_in_main = true;
	sim::bots::WalkerParams walker;
	sim::bots::PathService::Options paths;
	sim::NavQueryOptions query;
};

}

namespace bf3::host {

struct BotNav {
	std::shared_ptr<const BotLevel> level;
	std::unique_ptr<sim::NavQuery> query;
	std::unique_ptr<sim::bots::PathService> paths;
	sim::Tick run_tick = UINT32_MAX;
	double paths_ms = 0.0;

	void run_once(sim::Tick p_tick);

	sim::bots::WalkerView view(sim::Tick p_tick, uint32_t p_slot, const sim::SoldierState &p_self, std::span<const sim::bots::BotGoal> p_objectives,
			float p_stand_eye_height);
};

core::Result<std::unique_ptr<BotNav>> make_bot_nav(std::shared_ptr<const BotLevel> p_level, const sim::bots::PathService::Options &p_paths = {},
		const sim::NavQueryOptions &p_query = {});

}
