#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <array>
#include <list>
#include <string>

namespace bf3::ai {

class BotManager;

struct BotPlacement {
	std::string name;
	Vec3 position{};
	Vec3 rotation{};
	std::string fourth_argument;
	std::string sixth_argument;
};

class BotPlacementManager {
public:
	explicit BotPlacementManager(BotManager &r_bots) : bots_(r_bots) {}

	bool add_placement(const std::string &p_name, const Vec3 &p_position, const Vec3 &p_rotation,
			const std::string &p_fourth_argument, int p_team, const std::string &p_sixth_argument);
	bool spawn_bot(int p_team);
	const std::list<BotPlacement> &placements(int p_team) const { return placements_[size_t(p_team - 1)]; }

private:
	BotManager &bots_;
	std::array<std::list<BotPlacement>, 2> placements_;
};

}
