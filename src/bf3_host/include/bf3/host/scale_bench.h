#pragma once

#include "bf3/core/result.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/game_server.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::core {
class Cache;
class Install;
}

namespace bf3::host {

struct ScaleBenchOptions {
	std::string level = "MP_Subway";
	std::string mode = "ConquestLarge0";
	uint32_t walkers = 64;
	uint32_t observers = 1;
	double warmup_seconds = 10.0;
	double seconds = 120.0;
	unsigned load_threads = 0;
	core::Cache *cache = nullptr;
	uint64_t seed = 1;
	sim::bots::WalkerParams walker;
	std::string tuning_text;

	double paced_seconds = 30.0;
	bool raise_priority = true;

	bool gated = true;

	bool radius_round_up = true;
	float cell_size = 0.0f;
};

struct Percentiles {
	double p50 = 0.0, p95 = 0.0, p99 = 0.0, max = 0.0, mean = 0.0;
};

struct TailShares {
	uint64_t ticks = 0;
	double tick_ms = 0.0, commands_ms = 0.0, sources_ms = 0.0, step_ms = 0.0, rules_ms = 0.0, encode_ms = 0.0;
	double commands = 0.0, sources = 0.0, step = 0.0, rules = 0.0, encode = 0.0, other = 0.0;
};

struct ScaleReport {
	ScaleBenchOptions options;
	uint64_t ticks = 0;
	Percentiles tick_ms;
	Percentiles commands_ms, sources_ms, step_ms, rules_ms, encode_ms;

	double window_wall_seconds = 0.0;
	double window_cpu_seconds = 0.0;
	double cpu_share = 0.0;

	uint64_t paced_ticks = 0;
	Percentiles paced_tick_ms;

	bool bot_budget_ok = false;

	std::vector<double> bot_metres;
	uint32_t bots_walked = 0;
	double abandoned_share = 0.0;
	bool valid = false;

	uint32_t spawn_points_level = 0, spawn_points_in_main = 0, spawn_points_excluded = 0, spawn_points_synthetic = 0;
	bool spawn_teams_merged = false;
	TailShares tail;

	uint64_t motor_steps = 0;
	uint64_t collision_queries = 0;
	double queries_per_step = 0.0;
	double queries_per_second = 0.0;
	double us_per_step = 0.0;

	double us_per_query_upper = 0.0;

	uint32_t sweep_samples = 0;
	Percentiles us_sweep, us_overlap;

	sim::NavQueryStats query;
	double us_per_path = 0.0;
	double us_per_corridor_move = 0.0;
	double us_per_nearest = 0.0;

	sim::bots::PathServiceStats paths;
	double us_per_iteration = 0.0;
	double ticks_at_budget_share = 0.0;
	sim::bots::WalkerStats walkers;
	uint32_t respawns_in_window = 0;
	double seconds_init = 0.0;
	Percentiles spawn_tick_ms;
	uint64_t snapshot_bytes = 0;
	size_t max_snapshot_bytes = 0;
	uint32_t others_dropped_max = 0;

	double seconds_database = 0.0, seconds_areas = 0.0, seconds_shared = 0.0, seconds_bots = 0.0;
	sim::NavStats nav;
	uint64_t peak_working_set = 0;

	uint64_t server_working_set = 0;
	bool install_unchanged = true;
};

struct NavBakeBenchOptions {
	std::string level = "MP_007";
	std::string mode = "ConquestLarge0";
	unsigned threads = 16;
	uint32_t layer_mask = sim::query_mask::kMovement;
	bool radius_round_up = true;
	float cell_size = 0.0f;
	int partition = -1;
	bool link_checks = true;
	std::string tuning_text;
};
struct NavBakeBench {
	NavBakeBenchOptions options;
	sim::NavParams params;
	std::shared_ptr<const sim::NavWorld> nav;
	size_t tiles_requested = 0;
	size_t tiles_polygon = 0, tiles_point_only = 0;
	size_t tiles_terrain_only = 0;
	size_t tiles_with_objects = 0;
	size_t points_outside_areas = 0;

	size_t tiles_wet = 0;
	size_t triangles_unwalkable = 0;
	float water_y = 0.0f;
	size_t water_planes = 0;
	size_t soup_triangles = 0;
	double seconds_load = 0.0, seconds_areas = 0.0, seconds_soup = 0.0, seconds_cook = 0.0;
	double gate_seconds = 0.0;
	uint64_t tile_bytes_max = 0;
	double tile_bytes_mean = 0.0;
	uint64_t peak_working_set = 0;
};
core::Result<NavBakeBench> run_nav_bake_bench(std::shared_ptr<const core::Install> p_install, const NavBakeBenchOptions &p_options);

core::Result<std::vector<NavBakeBench>> run_nav_bake_benches(std::shared_ptr<const core::Install> p_install, const NavBakeBenchOptions &p_options,
		std::span<const unsigned> p_threads);
std::string format_nav_bake_bench(const NavBakeBench &p_bench);

inline constexpr double kNavBakeGateCaspianSeconds = 6.0;
inline constexpr double kNavBakeGateKhargSeconds = 13.0;

Percentiles percentiles_of(std::vector<double> p_values);
TailShares tail_shares(std::span<const GameServerTickTimes> p_ticks, double p_p99);

core::Result<ScaleReport> run_scale_bench(std::shared_ptr<const core::Install> p_install, const ScaleBenchOptions &p_options);

std::string format_scale_report(const ScaleReport &p_report);

inline constexpr double kScaleGateP99Ms = 8.0;

inline constexpr double kValidityMetres = 100.0;
inline constexpr double kValidityWalkedShare = 0.9;
inline constexpr double kValidityAbandonedShare = 0.1;

inline constexpr double kBotBudgetP99Ms = 2.0;
inline constexpr uint64_t kServerMemoryBudgetBytes = uint64_t(429496730);

inline constexpr double kNoisyCpuShare = 0.95;

}
