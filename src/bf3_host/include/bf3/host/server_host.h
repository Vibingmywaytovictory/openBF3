#pragma once
// ServerHost (plan §2.3): the authoritative server on its own std::thread (not Godot's
// WorkerThreadPool), at a fixed tick. Per tick: ENet service -> sim step -> publish counters.
// It calls no Godot API. The Godot side reads the counters (atomics) and a mutex-guarded copy of
// the body transforms; it must stop() the host before Godot's singletons go away (GameSession
// does so in NOTIFICATION_PREDELETE, the extension deinit calls stop_all()).

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

class NetEndpoint; // ENet server endpoint, defined in server_host.cpp

struct ServerConfig {
	int tick_hz = sim::PhysicsWorld::kTickHz;
	bool enable_net = true;
	const char *bind_ip = "127.0.0.1";
	uint16_t port = 0; // 0 = any free port
	size_t max_peers = 64;
	sim::WorldConfig world;
	// M1 (docs/design/m1.md §8): when set, the host runs a GameServer instead of the S0 dynamic-body world:
	// tick_hz must be sim::kSimHz, the ENet host opens net::kChannelCount channels and every datagram goes to
	// the GameServer. After GameServer::tick the host sends the tick's OutDatagrams and calls Host::flush() at
	// once, so a snapshot leaves in the tick that made it (the RTT bound of docs §7.5 depends on it; the S0 loop
	// flushes only after its step-1 service). Null keeps the S0 behaviour unchanged (sim.* / host.* tests).
	std::shared_ptr<const GameServerConfig> game;
};

// Plain copy of the counters, safe to read on any thread.
struct ServerStats {
	bool running = false;
	uint64_t ticks = 0;
	double last_step_ms = 0.0; // sim step of the last tick
	double p99_step_ms = 0.0; // over the last 600 ticks (10 s)
	double max_step_ms = 0.0;
	double last_tick_ms = 0.0; // whole tick work (net + sim + publish)
	double measured_hz = 0.0; // ticks per second since start
	uint64_t overruns = 0; // schedule resets after falling > 5 ticks behind
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
	~ServerHost(); // stops and joins
	ServerHost(const ServerHost &) = delete;
	ServerHost &operator=(const ServerHost &) = delete;

	bool start(const ServerConfig &p_config, std::string *r_error = nullptr);
	void stop(); // idempotent; joins the thread
	bool running() const { return running_.load(std::memory_order_acquire); }
	uint16_t port() const { return port_.load(std::memory_order_acquire); }
	ServerStats stats() const;
	// Latest published transforms of the dynamic bodies (copied under a short lock).
	void copy_body_transforms(std::vector<sim::BodyTransform> &r_out) const;
	// M1: the GameServer's stats as published after the last tick (empty when no game is configured). Declared
	// now, defined by track T4 (server_host.cpp): a caller cannot link before T4 lands.
	GameServerStats game_stats() const;

	// Stops and joins every running ServerHost of the process (extension deinit safety net).
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

	// Owned by the server thread while running.
	std::unique_ptr<sim::PhysicsWorld> world_;
	std::unique_ptr<NetEndpoint> net_;

	// Counters written by the server thread only.
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
	std::atomic<int> dynamic_bodies_{ 0 }; // read by stats() from any thread

	mutable std::mutex transforms_mutex_;
	std::vector<sim::BodyTransform> transforms_;

	// M1 T4 (config_.game set): the game replaces world_ on the server thread; its stats are published after every
	// tick under game_stats_mutex_ (game_stats()).
	std::unique_ptr<GameServer> game_;
	mutable std::mutex game_stats_mutex_;
	GameServerStats game_stats_;
};

} // namespace bf3::host
