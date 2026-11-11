#include "bf3/ai/main/bot_placement_manager.h"

#include "bf3/ai/main/bot_manager.h"

namespace bf3::ai {

namespace {

const std::string kPlaceholderName = "FIXME";

bool valid_team(int p_team) {
	return p_team == 1 || p_team == 2;
}

}

bool BotPlacementManager::add_placement(const std::string &p_name, const Vec3 &p_position, const Vec3 &p_rotation,
		const std::string &p_fourth_argument, int p_team, const std::string &p_sixth_argument) {
	if (!valid_team(p_team)) {
		return false;
	}
	placements_[size_t(p_team - 1)].push_back(
			BotPlacement{ p_name, p_position, p_rotation, p_fourth_argument, p_sixth_argument });
	return true;
}

bool BotPlacementManager::spawn_bot(int p_team) {
	if (!valid_team(p_team)) {
		return false;
	}
	std::list<BotPlacement> &team = placements_[size_t(p_team - 1)];
	if (team.empty()) {
		return false;
	}
	team.pop_front();
	Bot *const bot = bots_.last_created_bot();
	if (bot == nullptr) {
		return false;
	}
	bot->set_name(kPlaceholderName);
	return true;
}

}
