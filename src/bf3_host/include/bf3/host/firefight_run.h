#pragma once

#include "bf3/core/result.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/firefight_brain.h"
#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Cache;
class Install;
}

namespace bf3::host {

inline constexpr float kInsideToleranceMetres = 0.10f;
inline constexpr float kBelowProbeMetres = 200.0f;

struct FirefightLevel {
	std::shared_ptr<const SharedLevel> shared;
	std::shared_ptr<const BotLevel> bots;
	double seconds_database = 0, seconds_shared = 0, seconds_bots = 0;
	bool nav_cache_hit = false;
};

struct FirefightLoadOptions {
	core::Cache *cache = nullptr;
	unsigned threads = 0;
	sim::MotorTuning tuning;
	uint64_t seed = 1;
};

core::Result<FirefightLevel> load_firefight_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level, std::string_view p_mode,
		const FirefightLoadOptions &p_options);

struct FirefightRunOptions {
	FirefightMode mode = FirefightMode::Free;
	uint32_t count = 64;
	FirefightObserver observer;
	bool walk = true;
	uint64_t seed = 1;
	uint32_t warmup_ticks = 0;
	uint32_t ticks = 3600;
	uint32_t observers = 0;

	uint32_t max_slots = 64;
	uint32_t respawn_ticks = 60;
	bool hashes = true;
	uint32_t check_every = 60;
	uint32_t window_ticks = 1200;
	float stuck_metres = 1.0f;

	std::string mechanics_cfg;
	std::function<void(FirefightConfig &)> configure_firefight;
	std::function<void(GameServerConfig &)> configure_server;
};

struct FirefightBotRow {
	uint8_t slot = 0;
	uint8_t team = 0;
	uint8_t kit = 0;
	uint64_t shots = 0;
	uint32_t kills = 0;
	uint32_t deaths = 0;
	uint32_t lives = 0;
	uint64_t hits = 0;
	uint32_t windows = 0;
	uint32_t stuck_windows = 0;
	float min_window_metres = 0;
	double metres = 0;
};

struct FirefightPercentiles {
	double p50 = 0, p99 = 0, max = 0, mean = 0;
};
FirefightPercentiles firefight_percentiles(std::vector<double> p_samples);

struct FirefightCounters {
	uint32_t bots = 0, lanes = 0, lanes_rejected = 0, lanes_shared = 0, walk_unavailable = 0;
	uint64_t bursts = 0, reloads = 0, retargets = 0, no_target_fire = 0, placements = 0, los_rays = 0, hunts = 0, strafes = 0, mode_switches = 0,
			 sidearm_switches = 0;
};
FirefightCounters firefight_counters(const FirefightStats &p_stats);

struct FirefightReport {
	FirefightMode mode = FirefightMode::Free;
	uint32_t count = 0, observers = 0, max_slots = 0;
	uint64_t seed = 0, match_seed = 0, weapon_hash = 0;
	std::string level, mode_name;
	double seconds_init = 0, seconds_warmup = 0, seconds_window = 0;
	uint32_t ticks = 0;
	std::array<uint32_t, 3> kills{};
	std::array<uint32_t, 3> deaths{};
	std::array<uint64_t, 3> shots{};
	uint32_t team_kills = 0;
	uint64_t hits = 0;

	uint32_t peer_slots = 0;
	uint64_t peer_hits = 0;
	uint32_t peer_deaths = 0;
	uint8_t peer_team = 0;
	uint32_t peer_alive_ticks = 0, peer_protected_ticks = 0;

	uint32_t peer_checks = 0, peer_exposed = 0;
	std::vector<FirefightBotRow> bots;
	uint32_t bots_fired = 0, bots_hit_enemy = 0, bots_killed_enemy = 0, bots_stuck = 0, windows = 0, stuck_windows = 0;
	uint64_t checks = 0, inside_geometry = 0, below_ground = 0, non_finite = 0;
	uint64_t step_precondition_failures = 0, weapon_precondition_failures = 0;
	std::vector<uint64_t> tick_hashes;
	uint64_t run_hash = 0;
	FirefightPercentiles tick_ms, sources_ms, step_ms, rules_ms, encode_ms, weapons_ms, projectiles_ms, rewind_ms;
	uint32_t peak_projectiles = 0;
	FirefightCounters counters;
	GunplayStats gunplay;
	BotFillStats bot_fill;
};

core::Result<FirefightReport> run_firefight(std::shared_ptr<const SharedLevel> p_level, std::shared_ptr<const BotLevel> p_bots,
		const FirefightRunOptions &p_options);

std::string firefight_report_text(const FirefightReport &p_report);

}
