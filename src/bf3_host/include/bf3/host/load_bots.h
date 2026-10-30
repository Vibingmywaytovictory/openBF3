#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/net/netsim.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf3::host {

class ClientTransport;

class LegWalker final : public CommandSource {
public:
	static constexpr uint32_t kLegTicks = 2u * sim::kSimHz;

	explicit LegWalker(uint32_t p_phase) : phase_(p_phase) {}
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override;

private:
	uint32_t phase_ = 0;
	bool started_ = false;
	uint16_t base_yaw_ = 0;
	sim::Tick start_ = 0;
};

struct LoadBotConfig {
	std::string name = "loadbot";
	std::string level;
	std::string mode;
	std::optional<uint64_t> game_build;
	net::NetSimConfig netsim;
	uint8_t kit = 0;
	uint32_t spawn_retry_ms = 1000;
	uint32_t stall_level = 0;
};

struct LoadBotFleetConfig {
	std::string host = "127.0.0.1";
	uint16_t port = 0;
	uint32_t count = 1;
	uint32_t connect_timeout_ms = 5000;
	LoadBotConfig bot;
};

struct LoadBotStats {
	bool connected = false;
	bool matched = false;
	bool refused = false;
	bool disconnected = false;
	bool stalled = false;
	bool kicked = false;
	net::KickReason kick_reason = net::KickReason::Protocol;
	uint8_t slot = 0;
	uint32_t levels = 0;
	uint32_t level_changes = 0;
	uint32_t round_id = 0;
	uint32_t lives = 0;
	uint64_t datagrams_in = 0;
	uint64_t bytes_in = 0;
	uint64_t datagrams_out = 0;
	uint64_t bytes_out = 0;
	uint64_t snapshots = 0;
	uint64_t snapshots_lost = 0;
	uint64_t snapshot_others = 0;
	uint64_t shot_events = 0;
	uint64_t combat_events = 0;
	uint64_t reliable_messages = 0;
	uint64_t decode_errors = 0;
	uint64_t commands_sent = 0;
	uint64_t spawn_requests = 0;
	float max_walk_m = 0.0f;
	uint64_t connected_ns = 0;
	uint64_t ended_ns = 0;
	std::array<char, 128> last_error{};
};

struct LoadBotFleetStats {
	std::vector<LoadBotStats> bots;
	double seconds = 0.0;
	uint32_t matched = 0;
	uint32_t spawned = 0;
	uint32_t disconnected = 0;
	uint32_t refused = 0;
	uint32_t stalled = 0;
	uint32_t kicked = 0;
	uint32_t level_changes = 0;
	uint64_t snapshots = 0;
	uint64_t decode_errors = 0;
	uint64_t bytes_in = 0;
	uint64_t bytes_out = 0;
	double bytes_in_per_s = 0.0;
	double bytes_out_per_s = 0.0;
};

class LoadBotFleet {
public:
	LoadBotFleet();
	~LoadBotFleet();
	LoadBotFleet(const LoadBotFleet &) = delete;
	LoadBotFleet &operator=(const LoadBotFleet &) = delete;

	core::Status start(const LoadBotFleetConfig &p_config);
	core::Status start(const LoadBotFleetConfig &p_config, std::vector<std::unique_ptr<ClientTransport>> p_transports,
			std::function<uint64_t()> p_clock);
	void stop();
	bool running() const;
	LoadBotFleetStats stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
