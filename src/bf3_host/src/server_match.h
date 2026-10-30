#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/spawn.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bf3::host {

struct SquadOrderMessage {
	uint8_t team = 0;
	std::vector<uint8_t> bytes;
};

struct MatchBroadcast {
	std::vector<uint8_t> state;
	std::vector<uint8_t> players;
	std::vector<SquadOrderMessage> orders;
};

class MatchControl {
public:
	void configure(const GameServerConfig &p_config, size_t p_slots);
	core::Status create_rules(const GameServerConfig &p_config, uint64_t p_match_seed);

	bool on() const { return rules_ != nullptr; }
	TdmRules *rules() { return rules_.get(); }
	const TdmRules *rules() const { return rules_.get(); }
	RespawnPolicy &policy();

	uint8_t connect(size_t p_slot, bool p_bot, std::string_view p_name);
	uint8_t carry(size_t p_slot, std::string_view p_name, uint8_t p_team, int p_squad_mate);
	void leave(size_t p_slot);
	bool waits_for_deploy(size_t p_slot) const;
	sim::Command restrict(const sim::Command &p_command) const;
	size_t choose_spawn(size_t p_slot, std::span<const sim::SpawnPoint> p_points, const ServerView &p_view, sim::Tick p_tick) const;
	void request_spawn(size_t p_slot, uint8_t p_kit, uint8_t p_spawn_choice, sim::Tick p_tick);
	bool squad_spawn(size_t p_slot, std::span<const sim::SoldierState> p_live, sim::Vec3 &r_position, uint16_t &r_yaw);
	void request_suicide(size_t p_slot);
	void request_squad(size_t p_slot, const net::SquadRequest &p_request);
	bool take_suicide(size_t p_slot);

	void describe(net::MatchInfo &r_message) const;
	MatchBroadcast join_messages(sim::Tick p_tick, uint8_t p_team) const;
	MatchBroadcast take_broadcast(sim::Tick p_tick);

	void step(sim::Tick p_tick, const ServerView &p_view, const Gunplay &p_gunplay, std::span<sim::SoldierState> r_states);
	std::vector<RulesOrder> take_orders();

	uint64_t step_failures() const { return step_failures_; }

private:
	struct FixedRespawn final : RespawnPolicy {
		uint32_t ticks = 60;
		std::optional<uint32_t> respawn_tick(uint8_t, uint32_t p_death_tick) override { return p_death_tick + ticks; }
	};

	FixedRespawn fixed_;
	std::unique_ptr<TdmRules> rules_;
	size_t slots_ = 0;
	bool deploy_screen_ = false;
	uint8_t rules_flags_ = 0;
	uint64_t logic_digest_ = 0;
	std::vector<RulesOrder> orders_;
	sim::Tick player_list_tick_ = 0;
	bool player_list_sent_ = false;
	uint64_t step_failures_ = 0;
	uint64_t orders_revision_ = 0;
	std::array<std::array<SquadOrderState, net::kMaxSquads>, kSquadTeams> sent_orders_{};
};

}
