#pragma once
// The S1 server-scale measurement (spike S1 exit gates; plan §6 S1 row, D4, R1; docs/design/s1.md §1, §6): N walker
// bots on the real GameServer of a level x mode, ticked back to back on the calling thread for a fixed span of sim
// time, every tick timed. Godot-free; one code path for the ctest (host.scale) and `bf3dump bots`.
//
// Setup: open_level_database -> level_combat_tiles -> load_shared_level (terrain_cover = those tiles) ->
// load_bot_level -> GameServer::init with GameServerConfig::bots (count walkers) and max_slots 128, then
// `observers` synthetic peers connected through on_connect + Hello + Ready datagrams (no sockets, as
// host.session.unit drives GameServer), so the server encodes and "sends" a snapshot per observer every second tick.
// Observers send no commands: the server repeats their hold command (an idle soldier each).
// Run: warmup_seconds of ticks (the bots' initial spawns and their settles fall here and are reported separately),
// then seconds of measured ticks. GameServer::tick is called back to back (no sleeps, no ENet), so a tick's wall
// time is its CPU time on one thread, which is what the 16.7 ms budget of a 60 Hz ServerHost thread is spent on.
// A tick contains (game_server.h, GameServerStats parts): the commands from the jitter buffers, the command sources
// (PathService::run + every bot brain: corridor moves, corner lists, goal picks, nearest-poly queries), one motor
// step per live soldier (CharacterVirtual: contacts, sweeps, stairs, the motor's ground hug), the rules (respawns
// with settle_spawn, if any), and the snapshot encode for each observer.

#include "bf3/core/result.h"
#include "bf3/host/bot_fill.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::core {
class Cache;
class Install;
} // namespace bf3::core

namespace bf3::host {

struct ScaleBenchOptions {
	std::string level = "MP_Subway";
	std::string mode = "ConquestLarge0";
	uint32_t walkers = 64;
	uint32_t observers = 1; // the loopback human's snapshots (docs §6.1); more for the encode-scaling row
	double warmup_seconds = 10.0;
	double seconds = 120.0; // sim time measured (plan: at least 2 minutes)
	unsigned load_threads = 0; // soup, cook, nav bake lanes; 0 = all cores
	core::Cache *cache = nullptr; // null: everything cold, uncached
	uint64_t seed = 1;
	sim::bots::WalkerParams walker;
	std::string tuning_text; // motor_tuning.cfg contents ("" = the defaults)
};

struct Percentiles {
	double p50 = 0.0, p95 = 0.0, p99 = 0.0, max = 0.0, mean = 0.0;
};

struct ScaleReport {
	ScaleBenchOptions options;
	uint64_t ticks = 0; // measured
	Percentiles tick_ms; // GameServerStats::last_tick_ms over the measured ticks (the gate: p99)
	Percentiles commands_ms, sources_ms, step_ms, rules_ms, encode_ms;
	// Motor (docs §6.3): steps and narrow-phase queries in the window (CollisionWorld::query_count), queries per step,
	// queries per second of sim time, and the mean step cost (step_ms / steps per tick).
	uint64_t motor_steps = 0;
	uint64_t collision_queries = 0;
	double queries_per_step = 0.0;
	double queries_per_second = 0.0;
	double us_per_step = 0.0;
	// Nav (docs §6.4): calls and mean cost in the window, from the server NavQuery's counters.
	sim::NavQueryStats query;
	double us_per_path = 0.0; // ms_path / (paths_done + paths_partial + paths_failed)
	double us_per_corridor_move = 0.0;
	double us_per_nearest = 0.0;
	sim::bots::WalkerStats walkers; // in the window
	uint32_t respawns_in_window = 0; // ticks with a spawn inside the window are kept in tick_ms and counted here
	Percentiles spawn_tick_ms; // the warm-up ticks that spawned bots
	uint64_t snapshot_bytes = 0; // in the window, all observers
	size_t max_snapshot_bytes = 0;
	uint32_t others_dropped_max = 0; // others a snapshot could not carry (the 1,200-byte cap: about 55 others)
	// Load (reported, not gated): seconds of each step and whether the cache hit.
	double seconds_database = 0.0, seconds_areas = 0.0, seconds_shared = 0.0, seconds_bots = 0.0;
	sim::NavStats nav;
	uint64_t peak_working_set = 0;
	bool install_unchanged = true;
};

core::Result<ScaleReport> run_scale_bench(std::shared_ptr<const core::Install> p_install, const ScaleBenchOptions &p_options);
// The report as the text `bf3dump bots` and host.scale print (one "key value" per line, the gate rows marked).
std::string format_scale_report(const ScaleReport &p_report);
// The plan's p99 gate (docs §1 G1).
inline constexpr double kScaleGateP99Ms = 8.0;

} // namespace bf3::host
