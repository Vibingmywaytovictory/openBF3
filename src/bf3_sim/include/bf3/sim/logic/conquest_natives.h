#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/conquest_data.h"
#include "bf3/sim/logic/conquest_rules.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace bf3::sim::logic {

struct ConquestPlayerSample {
	uint8_t slot = 0;
	uint8_t team = 0;
	bool alive = false;
	bool ai = false;
	Vec3 position{};
};

struct ConquestPointView {
	uint32_t node = core::gameplay::kNoLogicIndex;
	uint32_t level_object = core::gameplay::kNoLogicIndex;
	uint8_t team = 0;
	uint8_t attacker = 0;
	float progress = 0.0f;
	float rate = 0.0f;
	bool controlled = false;
	bool under_attack = false;
	bool hq = false;
	int32_t ordinal = 0;
	Vec3 position{};
};

struct ConquestStats {
	uint64_t captures = 0;
	uint64_t losses = 0;
	uint64_t strikes = 0;
	uint64_t deserter_kills = 0;
	uint64_t ticket_respawn_costs = 0;
	uint64_t unhandled_inputs = 0;
	std::vector<std::pair<uint32_t, int32_t>> unhandled;
};

struct ConquestOptions {
	double game_mode_counter_modifier = 1.0;
	bool deserting_allowed = false;
	bool bleed_disabled = false;
};

class ConquestNatives {
public:
	static core::Result<ConquestNatives> create(std::shared_ptr<const core::gameplay::LogicGraph> p_graph,
			std::shared_ptr<const ConquestParams> p_params, const ConquestOptions &p_options);
	ConquestNatives(ConquestNatives &&) noexcept;
	ConquestNatives &operator=(ConquestNatives &&) noexcept;
	~ConquestNatives();

	bool handles(uint32_t p_node) const;
	core::Status effect(const LogicEffect &p_effect, LogicRuntime &r_runtime);
	core::Status step(
			Tick p_tick, float p_dt, std::span<const ConquestPlayerSample> p_players, LogicRuntime &r_runtime);

	void respawned(uint8_t p_slot, uint8_t p_team);
	void killed(uint8_t p_team);
	void left(uint8_t p_slot, uint8_t p_team);

	int64_t tickets(uint8_t p_team) const;
	int64_t initial_tickets(uint8_t p_team) const;
	bool bleeding(uint8_t p_team) const;
	bool spawn_allowed(uint8_t p_team) const;
	bool spawner_open(uint32_t p_node, uint8_t p_team) const;
	bool spawn_point_open(uint32_t p_spawner_object, uint8_t p_team) const;
	std::span<const ConquestPointView> points() const;
	std::span<const Deserter> deserters(uint8_t p_team) const;
	std::vector<uint8_t> take_kills();
	const ConquestStats &stats() const;

private:
	struct State;
	explicit ConquestNatives(std::unique_ptr<State> p_state);
	std::unique_ptr<State> state_;
};

}
