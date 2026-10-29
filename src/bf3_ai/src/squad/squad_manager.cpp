#include "bf3/ai/squad/squad_manager.h"

namespace bf3::ai {

SquadManager::SquadManager(const SquadServices &p_services) : services_(p_services) {
	const int squads = services_.world != nullptr ? services_.world->max_number_of_squads() : 1;
	for (int index = 0; index + 1 < squads; ++index) {
		coordinators_[0].push_back(std::make_unique<SquadCoordinator>(services_, 0, index));
		coordinators_[1].push_back(std::make_unique<SquadCoordinator>(services_, 1, index));
	}
}

void SquadManager::update(sim::Tick p_tick) {
	side_cursor_ = side_cursor_ == 0;
	if (coordinators_[0].empty()) {
		return;
	}
	coordinators_[size_t(side_cursor_)][squad_cursor_]->update(p_tick);
	if (side_cursor_ == 1) {
		if (squad_cursor_ + 1 < coordinators_[0].size()) {
			++squad_cursor_;
		} else {
			squad_cursor_ = 0;
			update_human_players(p_tick);
		}
	}
}

void SquadManager::set_player_on_valid_position(const SquadPlayer &p_player, bool p_valid, sim::Tick p_tick) {
	const int id = p_player.id();
	auto found = player_valid_positions_.find(id);
	if (p_valid) {
		if (found != player_valid_positions_.end()) {
			player_valid_positions_.erase(found);
		}
	} else if (found == player_valid_positions_.end()) {
		player_valid_positions_.emplace(id, p_tick);
	}
}

void SquadManager::update_human_players(sim::Tick p_tick) {
	if (services_.world == nullptr) {
		return;
	}
	for (const SquadPlayer *const player : services_.world->all_players()) {
		if (player == nullptr || player->is_ai()) {
			continue;
		}
		bool valid = true;
		const AiObject *const vehicle = player->current_vehicle();
		if (player->is_alive() && !player->is_man_down() && vehicle != nullptr) {
			valid = services_.world->is_valid_position(vehicle->position());
		}
		set_player_on_valid_position(*player, valid, p_tick);
	}
	for (auto it = player_valid_positions_.begin(); it != player_valid_positions_.end();) {
		const SquadPlayer *const player = services_.world->player(it->first);
		if (player == nullptr || player->is_ai() || !player->is_alive() || player->is_man_down()) {
			it = player_valid_positions_.erase(it);
		} else {
			++it;
		}
	}
}

void SquadManager::join(SquadCoordinator &r_squad, SquadPlayer &r_player) {
	r_squad.add_squad_member(r_player);
	SquadBot *const bot = services_.world->bot_from_player_id(r_player.id());
	if (bot != nullptr) {
		bot->join_squad(r_squad.squad_id() + 1);
	} else if (services_.squads != nullptr) {
		services_.squads->join_squad(r_squad.squad_id() + 1, r_player);
	}
	r_squad.update_squad_leader();
}

void SquadManager::add_squad_member(int p_player_id, int p_side) {
	if (services_.world == nullptr || p_side < 1 || p_side > 2) {
		return;
	}
	SquadPlayer *const player = services_.world->player(p_player_id);
	std::vector<std::unique_ptr<SquadCoordinator>> &coordinators = coordinators_[size_t(p_side - 1)];
	if (player == nullptr || coordinators.empty()) {
		return;
	}
	const int maximum = services_.world->max_number_of_squad_members();
	int best = maximum;
	SquadCoordinator *selected = nullptr;
	for (const std::unique_ptr<SquadCoordinator> &coordinator : coordinators) {
		const int members = coordinator->number_of_squad_members();
		if (members <= best && members > 0) {
			best = members;
			selected = coordinator.get();
		}
	}
	if (selected == nullptr) {
		selected = coordinators[0].get();
	}
	const size_t next = size_t(selected->squad_id()) + 1;
	if (next < coordinators.size() && coordinators[next]->number_of_squad_members() == 0) {
		best = selected->number_of_squad_members();
		if (best >= kSquadSplitSize) {
			SquadPlayer &moved = selected->squad_member(best - 1)->player();
			if (services_.squads != nullptr) {
				services_.squads->exit_squad(selected->squad_id() + 1, moved);
			}
			selected->remove_squad_member(moved.id());
			join(*coordinators[next], moved);
		}
	}
	join(*selected, *player);
}

bool SquadManager::remove_squad_member(int p_player_id, int p_side) {
	SquadCoordinator *const squad = squad_from_player(p_player_id, p_side);
	return squad != nullptr && squad->remove_squad_member(p_player_id);
}

void SquadManager::new_squad_leader(int p_side, int p_squad) {
	if (p_side < 1 || p_side > 2 || p_squad < 1 || p_squad > coordinator_count(p_side)) {
		return;
	}
	coordinator(p_side, p_squad).update_squad_leader();
}

SquadMember *SquadManager::squad_member(int p_player_id, int p_side) const {
	if (p_side < 1 || p_side > 2) {
		return nullptr;
	}
	for (const std::unique_ptr<SquadCoordinator> &coordinator : coordinators_[size_t(p_side - 1)]) {
		int index = 0;
		if (SquadMember *const member = coordinator->squad_member(p_player_id, index)) {
			return member;
		}
	}
	return nullptr;
}

SquadCoordinator *SquadManager::squad_from_player(int p_player_id, int p_side) const {
	if (p_side < 1 || p_side > 2) {
		return nullptr;
	}
	for (const std::unique_ptr<SquadCoordinator> &coordinator : coordinators_[size_t(p_side - 1)]) {
		int index = 0;
		if (coordinator->squad_member(p_player_id, index) != nullptr) {
			return coordinator.get();
		}
	}
	return nullptr;
}

void SquadManager::player_joins_squad(int p_player_id, int p_squad, int p_side) {
	if (p_squad == 0 || p_side < 1 || p_side > 2 || p_squad > coordinator_count(p_side) ||
			squad_member(p_player_id, p_side) != nullptr || services_.world == nullptr) {
		return;
	}
	SquadPlayer *const player = services_.world->player(p_player_id);
	if (player == nullptr) {
		return;
	}
	SquadCoordinator &squad = coordinator(p_side, p_squad);
	squad.add_squad_member(*player);
	squad.update_squad_leader();
}

int SquadManager::squad_strengths(int p_side, int p_area_index, int p_type) const {
	int total = 0;
	for (const std::unique_ptr<SquadCoordinator> &coordinator : coordinators_[size_t(p_side - 1)]) {
		if (coordinator->mission_status() == SquadMissionStatus::Accepted &&
				coordinator->is_squad_assigned_to_area(p_area_index)) {
			total += coordinator->strategic_strength(p_type);
		}
	}
	return total;
}

}
