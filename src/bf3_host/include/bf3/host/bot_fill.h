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
} // namespace bf3::core::level
namespace bf3::sim {
class CollisionWorld;
class SoldierMotor;
} // namespace bf3::sim

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
	// on_mesh and check.weak_component == the main component's weak component: a row with in_main_weak and not
	// in_main is reachable one way only (a drop), one with neither is truly disconnected (docs §5.5, the R6 class).
	bool in_main_weak = false;
	// on_mesh and its component can reach the main component (the main component itself included): a one-way row
	// that reaches it is exit-only (a spawn there feeds the main component), one that does not is a trap.
	bool reaches_main = false;
	bool settled = false; // spawns: settle_spawn succeeded (else the raw point was checked)
};

// Where the bots spawn (docs §8 "Bot spawns"; review of revision 2: G1 must not depend on G2). Built by
// load_bot_level from the Spawn rows, in this order:
//   1. every spawn point whose settled feet are in_main, with its own team;
//   2. if team 1 or team 2 then has no point (own team or neutral), every point of step 1 becomes neutral (team 0),
//      so both teams spawn on the main component (no combat in S1: team sides do not matter); teams_merged;
//   3. if step 1 found no point at all, `count` seeded NavQuery::random_points of the main component, settled with
//      settle_spawn on the server's world, as neutral points; synthetic. SpawnPoint::index = UINT32_MAX - i.
// GameServer::spawn() uses this list (not SharedLevel::spawns) for bot slots when BotFillConfig::spawn_in_main is
// set (choose_spawn over it, as for humans), so no bot starts outside the component its goals are in.
struct BotSpawns {
	std::vector<sim::SpawnPoint> points;
	uint32_t level_points = 0; // SharedLevel::spawns.size()
	uint32_t in_main = 0; // of those, settled feet in the main component
	uint32_t excluded = 0; // level_points - in_main: off the mesh or in another component
	bool teams_merged = false;
	uint32_t synthetic = 0; // step 3 points
};

struct BotLevel {
	std::string level;
	std::string mode;
	std::shared_ptr<const sim::NavWorld> nav;
	sim::NavComponents components;
	// The mode's main component: the most walkable area inside the mode's combat areas (mode_main_component;
	// implementation: on Metro the largest component of the region is not where TDM is played).
	uint32_t main_component = 0;
	std::vector<sim::bots::BotGoal> objectives; // the rows below that are in the main component, as goals
	std::vector<BotPointRow> points; // every flag, HQ, MCOM and spawn point of the mode
	BotSpawns spawns; // where bot slots spawn (main component only)
	std::vector<sim::NavTileKey> region; // the baked tiles
	double seconds_areas = 0.0; // level_combat_tiles (every MP mode traversed)
	double seconds_nav = 0.0; // load_nav_world_cached
	double seconds_components = 0.0;
};

struct BotLoadOptions {
	core::Cache *cache = nullptr;
	unsigned threads = 0; // nav bake lanes
	// The bake's params. With params_from_motor (the default) the soldier's numbers (radius, stand and crouch
	// heights, climb, slope) come from SharedLevel::motor (nav_params_from_motor) and the rest (cell sizes, radius
	// rounding, partition, drop links, margin) from here.
	sim::NavParams params;
	bool params_from_motor = true;
	uint32_t layer_mask = sim::query_mask::kMovement; // kAll only for the split investigation (docs §5.7)
	// BotSpawns step 3 (no spawn point in the main component): how many seeded main-component points to make.
	uint32_t synthetic_spawns = 64;
	uint64_t seed = 1; // of the synthetic points
	// The caller's level_combat_tiles (the one it built the terrain cover from), or null: then computed here (every
	// MP mode loaded and traversed again, about 0.3 s per mode).
	const core::level::LevelCombatTiles *combat = nullptr;
};

// On the loader thread that owns p_db (World is not thread-safe). p_shared must be the static world built with the
// same terrain cover: the call compares SharedLevel::terrain_cover with the level's combat-area tiles
// (level_combat_tiles) and fails otherwise. The nav bake's link checks use p_shared.static_world
// (NavBakeOptions::link_shapes, one CollisionWorld per bake lane).
core::Result<std::shared_ptr<const BotLevel>> load_bot_level(const LevelDatabase &p_db, const SharedLevel &p_shared, const BotLoadOptions &p_options,
		LoadProgress *p_progress);

// The pieces of load_bot_level, exposed for sim.nav (G2 per mode) and host.scale.unit (the spawn filter on synthetic
// worlds). docs §5.5 radii: spawn 1.0 m horizontal / 1.0 m vertical at settled feet, MCOM 2.0 m, flag and HQ 5.0 m
// (vertical 3.0 m).
float bot_point_radius(BotPointRow::Kind p_kind);
float bot_point_vertical(BotPointRow::Kind p_kind);
struct BotRowsResult {
	std::vector<BotPointRow> points; // objectives in order, then every spawn point
	std::vector<sim::bots::BotGoal> objectives; // the in_main rows as goals
	uint32_t main = 0; // the main component the rows were classified against
};
// Settles every spawn point with p_motor on p_world (the server's static world) and checks every row against
// p_main. With p_main_from_rows the main component is instead the one holding the most on-mesh rows (ties: p_main,
// else the larger area; no row on the mesh: p_main). Implementation (docs §5.5 as built): load_bot_level passes the
// mode's area-based main (mode_main_component) with p_main_from_rows, because on Metro the largest ground inside
// TDM's combat area is buried terrain under the street level that no spawn point touches.
BotRowsResult bot_point_rows(const sim::NavWorld &p_nav, const sim::NavComponents &p_components, uint32_t p_main,
		std::span<const core::level::ModeObjectiveDef> p_objectives, std::span<const sim::SpawnPoint> p_spawns, const sim::SoldierMotor &p_motor,
		const sim::CollisionWorld &p_world, bool p_main_from_rows = false);
// BotSpawns' three steps (above) from the rows.
BotSpawns make_bot_spawns(std::span<const BotPointRow> p_rows, std::span<const sim::SpawnPoint> p_spawns, const sim::NavComponents &p_components,
		uint32_t p_main, sim::NavQuery &p_query, const sim::SoldierMotor &p_motor, const sim::CollisionWorld &p_world, uint32_t p_synthetic, uint64_t p_seed);
// The weak component of the main SCC (UINT32_MAX when it has no poly).
uint32_t main_weak_component(const sim::NavComponents &p_components, uint32_t p_main);
// The component with the most ground area (poly centroids) inside p_areas' polygons; the largest when there are
// no areas or none holds any.
uint32_t mode_main_component(const sim::NavWorld &p_nav, const sim::NavComponents &p_components, std::span<const core::level::CombatAreaDef> p_areas);

struct BotFillConfig {
	std::shared_ptr<const BotLevel> level;
	uint32_t count = 0; // bots added at init
	// Bot slots spawn (and respawn) at level->spawns.points, every one in the main component (docs §8). False = the
	// mode's spawn points, as humans (M1's path; for the Godot run to show bots where humans spawn). The bench and
	// host.scale use true.
	bool spawn_in_main = true;
	sim::bots::WalkerParams walker;
	sim::bots::PathService::Options paths;
	sim::NavQueryOptions query;
};

// BotFillStats (GameServerStats::bots) is in bot_fill_stats.h.

} // namespace bf3::host
