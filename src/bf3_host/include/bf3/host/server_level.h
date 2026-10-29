#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bf3::core {
class Cache;
class Install;
}

namespace bf3::host {

struct BotLevel;

struct LevelEntry {
	std::string level;
	std::string mode;
	uint32_t rounds = 1;

	bool operator==(const LevelEntry &) const = default;
};

struct ServerLevelOptions {
	SharedLoadOptions shared;
	bool bots = false;
	uint32_t synthetic_spawns = 64;
	uint64_t seed = 1;
	bool keep_database = false;
	const std::atomic<bool> *stop = nullptr;
	std::function<core::Status(const LevelDatabase &, SharedLoadOptions &)> after_database;
	std::function<void(const std::shared_ptr<const SharedLevel> &)> after_shared;
};

struct ServerLevelSeconds {
	double database = 0.0;
	double combat = 0.0;
	double shared = 0.0;
	double bots = 0.0;
	double total = 0.0;
};

struct ServerLevel {
	LevelEntry entry;
	std::shared_ptr<const SharedLevel> shared;
	std::shared_ptr<const BotLevel> bots;
	LevelDatabase database;
	ServerLevelSeconds seconds;
	bool combat_cache_hit = false;
	bool nav_cache_hit = false;
};

inline constexpr const char *kServerLevelStopped = "server level load stopped";

core::Result<ServerLevel> load_server_level(std::shared_ptr<const core::Install> p_install, const LevelEntry &p_entry,
		const ServerLevelOptions &p_options, core::Cache *p_cache, LoadProgress *p_progress);

inline constexpr uint8_t kServerMaxPlayers = 64;
inline constexpr uint8_t kListenMaxPlayers = 32;
inline constexpr uint32_t kServerSlots = 64;

struct ServerRules {
	bool deploy_screen = true;
	bool revive = true;
	int kill_limit = 0;
	float preround_s = -1.0f;
	int min_players = -1;
	uint8_t max_players = kListenMaxPlayers;
	uint32_t bots = 31;
	std::string bot_tuning_cfg;
	std::string mechanics_cfg = GunplayConfig{}.mechanics_cfg;
	double respawn_s = 5.0;
	bool dev_commands = false;
	uint64_t seed = 1;
};

GameServerConfig tdm_game_server_config(const ServerLevel &p_level, const ServerRules &p_rules);

}
