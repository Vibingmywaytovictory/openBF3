#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_native.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::sim::logic {

struct RoundNativeParams {
	struct KillCounter {
		uint32_t node = core::gameplay::kNoLogicIndex;
		uint8_t team = 0;
		int32_t max_kill_count = 100;
		float enemy_weight = 1.0f;
		bool ignore_ai = false;
	};
	std::vector<KillCounter> kill_counters;

	int32_t round_min_players = 4;
	int32_t round_max_players = 8;
	float round_restart_countdown_s = 10.0f;

	float player_killed_delay_s = 5.0f;

	bool auto_balance = true;
	bool rotate_team_on_new_round = true;
	int32_t balance_player_count = 8;
	int32_t balance_team_difference = 3;

	uint32_t restrict_allowed = (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 8) | (1u << 9);
	bool from_data = false;
	std::vector<std::string> notes;
};

struct RoundNativeConfig {

	uint8_t max_players = 32;
	int kill_limit_override = 0;
	float preround_override_s = -1.0f;
	int32_t min_players_override = -1;

	std::shared_ptr<const RoundNativeParams> params;
	float team_kill_weight = 0.0f;
	bool rotate_teams = true;
	bool killer_camera = true;
};

enum class RoundPhase : uint8_t { PreRound = 0, InRound = 1, RoundOver = 2, Restarting = 3 };

class RoundNatives {
public:
	static core::Result<RoundNatives> create(std::shared_ptr<const core::gameplay::LogicGraph> p_graph, const RoundNativeConfig &p_config);
	RoundNatives(RoundNatives &&) noexcept;
	RoundNatives &operator=(RoundNatives &&) noexcept;
	~RoundNatives();

	core::Status apply(Tick p_tick, std::span<const LogicEffect> p_effects, LogicRuntime &r_runtime);

	core::Status player_enter(uint8_t p_slot, bool p_bot, LogicRuntime &r_runtime);
	core::Status player_leave(uint8_t p_slot, LogicRuntime &r_runtime);
	core::Status player_spawned(uint8_t p_slot, uint8_t p_team, LogicRuntime &r_runtime);

	core::Status player_died(uint8_t p_slot, uint8_t p_team, int p_killer, uint8_t p_killer_team, LogicRuntime &r_runtime);
	core::Status kill_undone(uint8_t p_killer_team, LogicRuntime &r_runtime);
	core::Status tick_round(Tick p_tick, LogicRuntime &r_runtime);

	core::Status player_enter_team(uint8_t p_slot, bool p_bot, uint8_t p_team, LogicRuntime &r_runtime);
	core::Status player_switch_team(uint8_t p_slot, uint8_t p_team);

	RoundPhase phase() const;
	uint8_t winner() const;
	int kills(uint8_t p_team) const;
	int kill_limit() const;
	bool input_restricted() const;
	uint32_t restriction_mask() const;
	Tick countdown_end_tick() const;
	uint8_t team_of(uint8_t p_slot) const;
	bool take_unspawn_all();
	bool take_restart();
	float player_killed_delay_s() const;
	bool killer_camera() const;
	std::span<const NativeTimelineEntry> timeline() const;

	uint32_t balance_moves() const;
	bool moves_at_death(uint8_t p_slot) const;
	uint32_t players() const;
	uint32_t min_players() const;
	uint32_t players_needed() const;
	const RoundNativeParams &params() const;

	struct NativeCoverage {
		core::gameplay::LogicTypeId type = core::gameplay::LogicTypeId::Unknown;
		uint32_t handled = 0;
		uint32_t recorded = 0;
	};
	std::span<const NativeCoverage> coverage() const;

private:
	struct State;
	explicit RoundNatives(std::unique_ptr<State> p_state);
	std::unique_ptr<State> state_;
};

}
