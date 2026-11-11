#include "bf3/ai/strategic/squad_commander.h"

#include "bf3/ai/squad/squad_manager.h"
#include "bf3/ai/strategic/strategy.h"

namespace bf3::ai {

SquadOrder squad_order_of(CommanderOrder p_order) {
	switch (p_order) {
		case CommanderOrder::Attack:
			return SquadOrder::Attack;
		case CommanderOrder::Defend:
			return SquadOrder::Defend;
		case CommanderOrder::Move:
			return SquadOrder::Move;
	}
	return SquadOrder::None;
}

void SquadCommanderGame::apply_for_commander(int, int) {}

void SquadCommanderGame::resign_commander(int) {}

void SquadCommanderGame::issue_commander_order(
		int p_side, int p_squad, CommanderOrder p_order, const Vec3 &p_position, uint16_t p_target) {
	const size_t area = size_t(p_target) - 1u;
	if ((p_side != 1 && p_side != 2) || p_squad < 1 || p_squad > squads_.coordinator_count(p_side) ||
			p_target == kNoCommanderTarget || area >= decisions_.area_count() ||
			squad_order_of(p_order) == SquadOrder::None) {
		++stats_.refused;
		return;
	}
	squads_.coordinator(p_side, p_squad)
			.assign_squad(&decisions_.area(area), p_position, squad_order_of(p_order), SquadMissionStatus::Issued);
	++stats_.orders;
}

void SquadCommanderGame::post_request(CommanderRequest, int, const Vec2 &) {}

void SquadCommanderGame::clear_request(int, int, CommanderRequest) {}

void SquadCommanderGame::play_commander_message(int, std::string_view) {}

bool SquadCommanderGame::has_artillery_pieces(int) const {
	return false;
}

bool SquadCommanderGame::have_any_artillery(int) const {
	return false;
}

}
