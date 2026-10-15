#pragma once
// Headless walker bots on the GameServer (spike S1, plan §3.6 "server-side fake clients emitting the same Command as
// humans", D11; docs/design/s1.md §6, §8). Godot-free.
//
//   load_bot_level   on the loader thread, after load_shared_level (built with SharedLoadOptions::terrain_cover):
//                    the mode's combat areas and objectives (bf3_core mode_areas.h), the level's combat-area tiles
//                    over every MP mode, the navmesh through the cache (sim nav.h), its components, and the goal set
//                    (flags, HQs, MCOMs, spawn points on the main component). Immutable after the load, shared.
//   GameServerConfig::bots   GameServer::init adds `count` server-side soldiers (add_local_soldier, teams 1 and 2
//                    alternating), each a CommandSource that forwards to one sim::bots::WalkerBrain; one BotDirector
//                    per GameServer owns the server thread's NavQuery and PathService and runs PathService::run once
//                    per tick before the first brain. Everything runs on the server thread (docs §3).

#include "bf3/core/result.h"
#include "bf3/sim/bots/walker_brain.h"
#include "bf3/sim/nav.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::core {
class Cache;
}

namespace bf3::host {

struct LevelDatabase;
struct SharedLevel;
struct LoadProgress;

// One gameplay point checked against the mesh (docs §5.5 connectivity gate).
struct BotPointRow {
	enum class Kind : uint8_t { Flag = 0, Hq = 1, Mcom = 2, Spawn = 3 };
	Kind kind = Kind::Spawn;
	uint32_t index = 0; // in its source list (mode_areas objectives / SharedLevel::spawns)
	sim::Vec3 point{}; // the data's position (spawns: settled feet, settle_spawn on the server's world)
	sim::NavPointCheck check;
	bool on_mesh = false; // check.poly != 0 and within the kind's radius (docs §5.5)
	bool in_main = false; // on_mesh and check.component == main component
};

struct BotLevel {
	std::string level;
	std::string mode;
	std::shared_ptr<const sim::NavWorld> nav;
	sim::NavComponents components;
	uint32_t main_component = 0; // the largest by area
	std::vector<sim::bots::BotGoal> objectives; // the rows below that are in the main component, as goals
	std::vector<BotPointRow> points; // every flag, HQ, MCOM and spawn point of the mode
	std::vector<sim::NavTileKey> region; // the baked tiles
	double seconds_areas = 0.0; // level_combat_tiles (every MP mode traversed)
	double seconds_nav = 0.0; // load_nav_world_cached
	double seconds_components = 0.0;
};

struct BotLoadOptions {
	core::Cache *cache = nullptr;
	unsigned threads = 0; // nav bake lanes
	sim::NavParams params; // nav_params_from_motor(SharedLevel::motor) by default (load_bot_level fills it when unset)
	bool params_from_motor = true;
};

// On the loader thread that owns p_db (World is not thread-safe). p_shared must be the static world built with the
// same terrain cover (the call checks the cover's digest against the static world's options and fails otherwise).
core::Result<std::shared_ptr<const BotLevel>> load_bot_level(const LevelDatabase &p_db, const SharedLevel &p_shared, const BotLoadOptions &p_options,
		LoadProgress *p_progress);

struct BotFillConfig {
	std::shared_ptr<const BotLevel> level;
	uint32_t count = 0; // bots added at init
	sim::bots::WalkerParams walker;
	sim::bots::PathService::Options paths;
	sim::NavQueryOptions query;
};

// Per tick and cumulative bot counters (GameServerStats::bots). Trivially copyable.
struct BotFillStats {
	uint32_t bots = 0;
	uint32_t thinks_last_tick = 0;
	uint32_t path_iterations_last_tick = 0;
	uint32_t paths_pending = 0;
	double last_brains_ms = 0.0; // PathService::run + every brain's command() of the last tick
	double last_paths_ms = 0.0; // of which PathService::run
	sim::bots::WalkerStats walkers; // summed over the bots, cumulative
	sim::NavQueryStats query; // the server NavQuery's cumulative counters
};

} // namespace bf3::host
