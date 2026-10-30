#pragma once

#include "bf3/core/result.h"
#include "bf3/host/bot_fill_stats.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::host {

struct BotFillConfig;

inline constexpr size_t kKickReasonCount = size_t(net::KickReason::LoadStalled) + 1;
using KickCounts = std::array<uint64_t, kKickReasonCount>;
const char *kick_reason_name(net::KickReason p_code);
std::string kick_counts_text(const KickCounts &p_kicks);
std::string install_difference_text(uint8_t p_components);

struct GameServerTickTimes {
	sim::Tick tick = 0;
	double tick_ms = 0, commands_ms = 0, sources_ms = 0, step_ms = 0, rules_ms = 0, encode_ms = 0;
	double paths_ms = 0;
	uint32_t motor_steps = 0;
	uint32_t spawns = 0;
	uint32_t snapshots = 0;
	uint64_t collision_queries = 0;
};

class CommandSource {
public:
	virtual ~CommandSource() = default;
	virtual sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) = 0;
};

struct GameServerConfig {
	std::shared_ptr<const SharedLevel> level;
	uint8_t snapshot_divisor = 2;
	uint32_t snapshot_budget_bytes = uint32_t(net::kMaxDatagramBytes);

	uint32_t max_slots = 64;
	uint32_t jitter_buffer_ticks = 32;
	uint32_t respawn_ticks = 60;
	uint32_t walkers = 0;
	bool dev_commands = false;
	uint64_t seed = 1;
	std::optional<uint64_t> game_build;

	std::shared_ptr<const BotFillConfig> bots;

	GunplayConfig gunplay;

	std::shared_ptr<const struct FirefightConfig> firefight;

	MatchRulesConfig rules;
	std::shared_ptr<const struct TdmBotConfig> tdm_bots;
};

struct OutDatagram {
	uint32_t peer = 0;
	uint8_t channel = 0;
	bool reliable = false;
	std::vector<uint8_t> bytes;
};

struct SlotStats {
	bool used = false;
	bool remote = false;
	uint32_t peer = 0;
	uint32_t life = 0;
	bool alive = false;
	uint64_t commands_applied = 0;
	uint64_t commands_repeated = 0;
	uint64_t commands_late = 0;
	uint64_t commands_replaced = 0;

	int32_t buffer_ticks = 0;

	sim::SoldierState state;
};

struct GameServerStats {
	sim::Tick tick = 0;
	uint32_t clients = 0;
	uint32_t soldiers = 0;
	uint64_t snapshots_sent = 0;
	uint64_t snapshot_bytes = 0;
	size_t max_snapshot_bytes = 0;
	uint64_t bad_datagrams = 0;
	uint64_t spawn_failures = 0;
	KickCounts kicks{};
	uint64_t rules_step_failures = 0;
	bool rules_active = false;
	net::MatchState match;
	RulesStats rules;

	uint64_t step_precondition_failures = 0;
	double last_step_ms = 0;
	double last_encode_ms = 0;
	std::vector<SlotStats> slots;

	double last_tick_ms = 0;
	double last_commands_ms = 0;
	double last_sources_ms = 0;
	double last_rules_ms = 0;
	uint64_t motor_steps = 0;
	uint64_t collision_queries = 0;
	BotFillStats bots;

	bool gunplay_active = false;
	GunplayStats gunplay;
	uint64_t match_seed = 0;
	uint64_t weapon_hash = 0;
	uint64_t kit_hash = 0;
	uint8_t weapon_flags = 0;
};

class GameServer {
public:
	GameServer();
	~GameServer();
	GameServer(const GameServer &) = delete;
	GameServer &operator=(const GameServer &) = delete;

	core::Status init(const GameServerConfig &p_config);
	void on_connect(uint32_t p_peer);
	void on_disconnect(uint32_t p_peer);
	void on_receive(uint32_t p_peer, uint8_t p_channel, std::span<const uint8_t> p_datagram);

	void tick(std::vector<OutDatagram> &r_out);
	sim::Tick current_tick() const;

	int add_local_soldier(std::unique_ptr<CommandSource> p_source, int p_team);

	int add_local_bot(std::unique_ptr<CommandSource> p_source, int p_team);
	GameServerStats stats() const;

	const GameServerTickTimes &last_tick_times() const;

	const sim::SoldierState *soldier(uint32_t p_slot) const;

	core::Status place_soldier(uint32_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose);

	const ServerView &view() const;

	const Gunplay *gunplay() const;

	const TdmRules *rules() const;

	bool set_squad_order(uint8_t p_team, uint8_t p_squad, const SquadOrderState &p_order);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
