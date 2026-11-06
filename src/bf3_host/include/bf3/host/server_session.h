#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/host/map_list.h"
#include "bf3/host/rotation.h"
#include "bf3/host/server_host.h"
#include "bf3/host/server_level.h"
#include "bf3/host/stuck_bots.h"
#include "bf3/net/master.h"

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
	float intermission_s = -1.0f;
	std::shared_ptr<TickClock> clock;
	std::string master;
	std::string name;
	uint32_t master_heartbeat_ms = net::kMasterHeartbeatMs;
};

struct PrebakeEntry {
	enum class State : uint8_t { Pending = 0, Running = 1, Done = 2, Failed = 3 };
	LevelEntry entry;
	State state = State::Pending;
	ServerLevelSeconds seconds;
	bool nav_cache_hit = false;
	std::string error;
};
const char *prebake_state_name(PrebakeEntry::State p_state);
const char *match_phase_name(net::MatchPhase p_phase);

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
	const char *brain = "none";
	uint64_t bot_decisions = 0;
	uint64_t squad_samples = 0;
	uint64_t squad_near = 0;
	float squad_median_m = 0.0f;
	uint64_t squad_orders = 0;
	uint64_t squad_spawns = 0;
	bool master = false;
	net::MasterHeartbeatStats heartbeat;
};

uint64_t process_private_bytes();
uint64_t process_peak_private_bytes();
bool release_free_heap_memory();

class ServerSession {
public:
	ServerSession();
	~ServerSession();
	ServerSession(const ServerSession &) = delete;
	ServerSession &operator=(const ServerSession &) = delete;

	core::Status start(ServerSessionConfig p_config, LoadProgress *p_progress = nullptr);
	void stop();
	void request_stop() { stop_.store(true, std::memory_order_release); }

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
	net::MasterHeartbeat heartbeat() const;

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

	StuckBots stuck_;

	net::MasterHeartbeater heartbeater_;
};

}
