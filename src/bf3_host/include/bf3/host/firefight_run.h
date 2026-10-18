#pragma once
// A headless firefight on the real GameServer (docs/design/m2a.md §10.2, §14.1, E13, E15; owner bt; as built:
// docs/design/m2a-bt.md). Godot-free; used by `bf3dump firefight` and tests/host_gunplay_perf.cpp
// (host.gunplay.firefight, host.gunplay.replay, host.gunplay.perf).
//
//   load_firefight_level  on the caller's thread: the level database, load_shared_level with the gunplay tables
//                         (SharedLoadOptions::gunplay) and S1's bot level (nav through the cache), the database
//                         dropped at the end. Read-only on the install; the cache is the caller's (null: none).
//   run_firefight         GameServer::init with GameServerConfig::firefight (bt's brains through the G3 hook) and
//                         GameServerConfig::bots (count 0: the BotLevel only, for walking), optional idle observer
//                         peers (Hello + Ready, as S1's scale bench: they receive the snapshots, shots and combat
//                         events, so encode runs as in a match), then warm-up and window ticks back to back on the
//                         caller's thread (no ServerHost, no sockets, no wall-clock wait). It records, without
//                         touching the game:
//     * kills by the killer's team, deaths, hits, shots per bot (GunplayConfig::observer, so the run sets
//       dev_commands; nothing else of the dev set is used);
//     * per bot: displacement windows of window_ticks while alive on one life (stuck: the farthest horizontal
//       distance from the window's start position below stuck_metres);
//     * every check_every ticks per live soldier: inside geometry (the pose's upright capsule overlaps the static world
//       deeper than kInsideToleranceMetres), below the ground (no surface within kBelowProbeMetres under the feet), a
//       non-finite state;
//     * GameServer's and Gunplay's precondition counters;
//     * per tick: GameServerTickTimes and Gunplay's weapons / projectiles / rewind ms; with hashes, one world hash
//       (every used slot's soldier_state_hash and soldier_weapons_hash, the live projectiles field by field, the tick's
//       damage events and their outcome, its impacts and detonations; FNV-1a 64 in slot order).
// Wall-clock times are reported, never waited on; the E13 gate reads them (timing row, quiet lock).

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
} // namespace bf3::core

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
	unsigned threads = 0; // soup, cook and nav bake threads (0 = all cores)
	sim::MotorTuning tuning;
	uint64_t seed = 1; // BotLoadOptions::seed (synthetic spawns only)
};

core::Result<FirefightLevel> load_firefight_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level, std::string_view p_mode,
		const FirefightLoadOptions &p_options);

struct FirefightRunOptions {
	FirefightMode mode = FirefightMode::Free;
	uint32_t count = 64;
	FirefightObserver observer; // Pairs; invalid = the level's range lane
	bool walk = true;
	uint64_t seed = 1; // GameServerConfig::seed (the match seed derives from it)
	uint32_t warmup_ticks = 0;
	uint32_t ticks = 3600; // the measured window
	uint32_t observers = 0; // idle peers
	// 64: lag_comp.cpp's RewindProvider answers hit targets for slots 0 .. 63 only (kMaxLagSlots, 64-bit masks), so a
	// soldier in slot 64 or above is never hit (measured: an observer peer in slot 64 drew 0 impacts in 60 s while bots
	// fired at it). A run that needs more slots (E13's observer beside 64 bots) says so and accepts it.
	uint32_t max_slots = 64;
	uint32_t respawn_ticks = 60;
	bool hashes = true;
	uint32_t check_every = 60; // 0 = no geometry / NaN checks
	uint32_t window_ticks = 1200; // 20 s
	float stuck_metres = 1.0f;
	// GunplayConfig::mechanics_cfg (empty: its default, config/weapon_mechanics.cfg relative to the working directory, the
	// Godot project's; tools and tests pass the file in bf3-opensource/config).
	std::string mechanics_cfg;
	std::function<void(FirefightConfig &)> configure_firefight; // after the defaults above
	std::function<void(GameServerConfig &)> configure_server;
};

struct FirefightBotRow {
	uint8_t slot = 0;
	uint8_t team = 0;
	uint8_t kit = 0;
	uint64_t shots = 0;
	uint32_t kills = 0; // enemies killed
	uint32_t deaths = 0;
	uint32_t lives = 0;
	uint64_t hits = 0; // damage events applied to an enemy
	uint32_t windows = 0; // full displacement windows
	uint32_t stuck_windows = 0;
	float min_window_metres = 0; // the smallest window displacement (0 without a full window)
	double metres = 0; // horizontal path length (steps under 5 m)
};

struct FirefightPercentiles {
	double p50 = 0, p99 = 0, max = 0, mean = 0;
};
FirefightPercentiles firefight_percentiles(std::vector<double> p_samples); // nearest rank, as S1's bench

// FirefightStats as plain numbers.
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
	uint32_t ticks = 0; // window ticks run
	std::array<uint32_t, 3> kills{}; // by the killer's team (1, 2); [0] = killers without a team
	std::array<uint32_t, 3> deaths{}; // by the victim's team, every cause (fall deaths included)
	std::array<uint64_t, 3> shots{}; // by the shooter's team
	uint32_t team_kills = 0; // must stay 0 (friendly fire off)
	uint64_t hits = 0; // applied damage events
	// The observer peers (soldiers of remote slots that send no command: the server holds them at their spawn). A bot
	// that engages a human hits one of these (the gx playable scene's rule: a human is an enemy like any other).
	uint32_t peer_slots = 0;
	uint64_t peer_hits = 0; // applied damage events on a peer from a firefight bot
	uint32_t peer_deaths = 0;
	uint8_t peer_team = 0; // the first peer's
	uint32_t peer_alive_ticks = 0, peer_protected_ticks = 0; // the first peer's window ticks alive / spawn protected
	// Every check_every ticks while the first peer is alive: the checks, and those where an enemy bot within 150 m had a
	// clear eye-to-chest ray to it (kProjectile), so a peer that is never exposed is told from one that is ignored.
	uint32_t peer_checks = 0, peer_exposed = 0;
	std::vector<FirefightBotRow> bots; // the firefight bots, slot order
	uint32_t bots_fired = 0, bots_hit_enemy = 0, bots_killed_enemy = 0, bots_stuck = 0, windows = 0, stuck_windows = 0;
	uint64_t checks = 0, inside_geometry = 0, below_ground = 0, non_finite = 0;
	uint64_t step_precondition_failures = 0, weapon_precondition_failures = 0;
	std::vector<uint64_t> tick_hashes; // one per window tick (hashes on)
	uint64_t run_hash = 0; // FNV-1a over tick_hashes
	FirefightPercentiles tick_ms, sources_ms, step_ms, rules_ms, encode_ms, weapons_ms, projectiles_ms, rewind_ms;
	uint32_t peak_projectiles = 0;
	FirefightCounters counters;
	GunplayStats gunplay;
	BotFillStats bot_fill; // GameServerStats::bots (zero: the firefight's own BotNav is not in it)
};

// p_bots may be null (no walking). Fails on a level without gunplay tables.
core::Result<FirefightReport> run_firefight(std::shared_ptr<const SharedLevel> p_level, std::shared_ptr<const BotLevel> p_bots,
		const FirefightRunOptions &p_options);
// The report as printed by bf3dump firefight and the tests (several lines, no trailing newline).
std::string firefight_report_text(const FirefightReport &p_report);

} // namespace bf3::host
