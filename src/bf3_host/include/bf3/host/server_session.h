#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/host/map_list.h"
#include "bf3/host/rotation.h"
#include "bf3/host/server_host.h"
#include "bf3/host/server_level.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bf3::core {
class Cache;
class Install;
}

namespace bf3::host {

struct ServerSessionConfig {
	std::shared_ptr<const core::Install> install;
	std::shared_ptr<core::Cache> cache;
	MapList maps;
	ServerLevelOptions level;
	ServerRules rules;
	std::string bind_ip = "127.0.0.1";
	uint16_t port = 0;
	bool prebake = true;
	LoadDeadlines load;
	std::shared_ptr<TickClock> clock;
};

struct PrebakeEntry {
	enum class State : uint8_t { Pending = 0, Running = 1, Done = 2, Failed = 3 };
	LevelEntry entry;
	State state = State::Pending;
	ServerLevelSeconds seconds;
	bool combat_cache_hit = false;
	bool nav_cache_hit = false;
	std::string error;
};
const char *prebake_state_name(PrebakeEntry::State p_state);
const char *match_phase_name(net::MatchPhase p_phase);

inline constexpr uint32_t kStuckWindowTicks = 1200;
inline constexpr float kStuckMetres = 1.0f;

struct ServerSessionStats {
	ServerStats server;
	GameServerStats game;
	std::vector<PrebakeEntry> prebake;
	uint64_t private_bytes = 0;
	uint64_t peak_private_bytes = 0;
	uint32_t stuck_bots = 0;
	uint32_t bots = 0;
	int32_t kills = 0;
	RotationStats rotation;
};

uint64_t process_private_bytes();
uint64_t process_peak_private_bytes();

class ServerSession {
public:
	ServerSession();
	~ServerSession();
	ServerSession(const ServerSession &) = delete;
	ServerSession &operator=(const ServerSession &) = delete;

	core::Status start(ServerSessionConfig p_config, LoadProgress *p_progress = nullptr);
	void stop();

	bool running() const;
	uint16_t port() const;
	const ServerLevel &level() const { return *level_; }
	const MapListCursor &cursor() const { return *cursor_; }
	const GameServerConfig &game_config() const { return *game_; }
	ServerHost &host() { return host_; }

	ServerSessionStats stats();
	std::vector<PrebakeEntry> prebake() const;
	bool wait_prebake(std::chrono::milliseconds p_timeout) const;

private:
	void run_prebake();
	GameServerConfig rotation_config(const ServerLevel &p_level) const;
	void track_stuck(const GameServerStats &p_game);

	struct StuckTrack {
		uint32_t life = 0;
		sim::Tick since = 0;
		sim::Vec3 at{};
		bool counted = false;
		bool alive = false;
	};

	ServerSessionConfig config_;
	std::unique_ptr<ServerLevel> level_;
	std::unique_ptr<MapListCursor> cursor_;
	std::shared_ptr<const GameServerConfig> game_;
	std::shared_ptr<Rotation> rotation_;
	ServerHost host_;

	std::thread prebake_thread_;
	std::atomic<bool> stop_{ false };
	mutable std::mutex prebake_mutex_;
	mutable std::condition_variable prebake_done_;
	std::vector<PrebakeEntry> prebake_;
	bool prebake_finished_ = true;

	std::vector<StuckTrack> stuck_;
	uint32_t stuck_bots_ = 0;
};

}
