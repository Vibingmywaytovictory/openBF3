#pragma once

#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/squad/squad_coordinator.h"

#include <array>
#include <map>
#include <memory>
#include <vector>

namespace bf3::ai {

inline constexpr int kSquadSplitSize = 3;

class SquadManager final : public SquadUpdater {
public:
	explicit SquadManager(const SquadServices &p_services);
	SquadManager(const SquadManager &) = delete;
	SquadManager &operator=(const SquadManager &) = delete;

	void update(sim::Tick p_tick) override;
	void new_squad_leader(int p_side, int p_squad) override;
	void player_joins_squad(int p_player_id, int p_squad, int p_side) override;

	void update_human_players(sim::Tick p_tick);
	void set_player_on_valid_position(const SquadPlayer &p_player, bool p_valid, sim::Tick p_tick);
	void add_squad_member(int p_player_id, int p_side);
	bool remove_squad_member(int p_player_id, int p_side);
	SquadMember *squad_member(int p_player_id, int p_side) const;
	SquadCoordinator *squad_from_player(int p_player_id, int p_side) const;
	int squad_strengths(int p_side, int p_area_index, int p_type) const;
	void clear_player_valid_positions() { player_valid_positions_.clear(); }

	int coordinator_count(int p_side) const { return int(coordinators_[size_t(p_side - 1)].size()); }
	SquadCoordinator &coordinator(int p_side, int p_squad) const {
		return *coordinators_[size_t(p_side - 1)][size_t(p_squad - 1)];
	}
	const std::map<int, sim::Tick> &player_valid_positions() const { return player_valid_positions_; }

private:
	void join(SquadCoordinator &r_squad, SquadPlayer &r_player);

	SquadServices services_;
	std::array<std::vector<std::unique_ptr<SquadCoordinator>>, 2> coordinators_;
	size_t squad_cursor_ = 0;
	int side_cursor_ = 1;
	std::map<int, sim::Tick> player_valid_positions_;
};

}
