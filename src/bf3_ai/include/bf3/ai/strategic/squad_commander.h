#pragma once

#include "bf3/ai/commander/commander.h"
#include "bf3/ai/squad/squad_coordinator.h"

#include <cstdint>
#include <string_view>

namespace bf3::ai {

class SquadManager;
class StrategyDecisions;

struct SquadCommanderStats {
	uint64_t orders = 0;
	uint64_t refused = 0;
};

SquadOrder squad_order_of(CommanderOrder p_order);

class SquadCommanderGame final : public CommanderGame {
public:
	SquadCommanderGame(SquadManager &r_squads, const StrategyDecisions &p_decisions) :
			squads_(r_squads), decisions_(p_decisions) {}

	void apply_for_commander(int p_side, int p_player_id) override;
	void resign_commander(int p_side) override;
	void issue_commander_order(
			int p_side, int p_squad, CommanderOrder p_order, const Vec3 &p_position, uint16_t p_target) override;
	void post_request(CommanderRequest p_request, int p_side, const Vec2 &p_position) override;
	void clear_request(int p_side, int p_squad, CommanderRequest p_request) override;
	void play_commander_message(int p_side, std::string_view p_name) override;
	bool has_artillery_pieces(int p_side) const override;
	bool have_any_artillery(int p_side) const override;

	const SquadCommanderStats &stats() const { return stats_; }

private:
	SquadManager &squads_;
	const StrategyDecisions &decisions_;
	SquadCommanderStats stats_;
};

}
