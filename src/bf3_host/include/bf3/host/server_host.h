#pragma once

#include "bf3/host/game_server.h"
#include "bf3/sim/physics_world.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bf3::host {

class NetEndpoint;

struct ServerConfig {
	int tick_hz = sim::PhysicsWorld::kTickHz;
	bool enable_net = true;
	const char *bind_ip = "127.0.0.1";
	uint16_t port = 0;
	size_t max_peers = 64;
	sim::WorldConfig world;

	std::shared_ptr<const GameServerConfig> game;
};

struct ServerStats {
	bool running = false;
	uint64_t ticks = 0;
	double last_step_ms = 0.0;
	double p99_step_ms = 0.0;
	double max_step_ms = 0.0;
	double last_tick_ms = 0.0;
	double measured_hz = 0.0;
	uint64_t overruns = 0;
	uint64_t state_hash = 0;
	int dynamic_bodies = 0;
	int active_bodies = 0;
	float lowest_body_y = 0.0f;
	uint16_t port = 0;
	uint32_t peers = 0;
	uint64_t pings_echoed = 0;
	uint64_t step_errors = 0;
};

class ServerHost {
public:
	ServerHost();
	~ServerHost();
	ServerHost(const ServerHost &) = delete;
	ServerHost &operator=(const ServerHost &) = delete;

	bool start(const ServerConfig &p_config, std::string *r_error = nullptr);
	void stop();
	bool running() const { return running_.load(std::memory_order_acquire); }
	uint16_t port() const { return port_.load(std::memory_order_acquire); }
	ServerStats stats() const;

	void copy_body_transforms(std::vector<sim::BodyTransform> &r_out) const;

	GameServerStats game_stats() const;

	static size_t stop_all();
	static size_t live_count();

private:
	void run();

	ServerConfig config_;
	std::thread thread_;
	std::mutex lifecycle_mutex_;
	std::atomic<bool> stop_requested_{ false };
	std::atomic<bool> running_{ false };
	std::atomic<uint16_t> port_{ 0 };

	std::unique_ptr<sim::PhysicsWorld> world_;
	std::unique_ptr<NetEndpoint> net_;

	std::atomic<uint64_t> ticks_{ 0 };
	std::atomic<uint32_t> last_step_us_{ 0 };
	std::atomic<uint32_t> p99_step_us_{ 0 };
	std::atomic<uint32_t> max_step_us_{ 0 };
	std::atomic<uint32_t> last_tick_us_{ 0 };
	std::atomic<uint64_t> overruns_{ 0 };
	std::atomic<uint64_t> state_hash_{ 0 };
	std::atomic<int> active_bodies_{ 0 };
	std::atomic<float> lowest_body_y_{ 0.0f };
	std::atomic<uint32_t> peers_{ 0 };
	std::atomic<uint64_t> pings_echoed_{ 0 };
	std::atomic<uint64_t> step_errors_{ 0 };
	std::atomic<int64_t> start_ns_{ 0 };
	std::atomic<int> dynamic_bodies_{ 0 };

	mutable std::mutex transforms_mutex_;
	std::vector<sim::BodyTransform> transforms_;

	std::unique_ptr<GameServer> game_;
	mutable std::mutex game_stats_mutex_;
	GameServerStats game_stats_;
};

}
