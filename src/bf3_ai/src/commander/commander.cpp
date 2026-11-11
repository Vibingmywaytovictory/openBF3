#include "bf3/ai/commander/commander.h"

namespace bf3::ai {

void BFCommander::apply_as_commander(int p_player_id, int p_side) {
	game_.apply_for_commander(p_side, p_player_id);
}

void BFCommander::retire_as_commander(int p_side) {
	game_.resign_commander(p_side);
}

void BFCommander::give_attack_order(const Vec3 &p_position, int p_squad, int p_side, uint16_t p_target) {
	game_.issue_commander_order(p_side, p_squad, CommanderOrder::Attack, p_position, p_target);
}

void BFCommander::give_move_order(const Vec3 &p_position, int p_squad, int p_side, uint16_t p_target) {
	game_.issue_commander_order(p_side, p_squad, CommanderOrder::Move, p_position, p_target);
}

void BFCommander::give_defend_order(const Vec3 &p_position, int p_squad, int p_side, uint16_t p_target) {
	game_.issue_commander_order(p_side, p_squad, CommanderOrder::Defend, p_position, p_target);
}

void BFCommander::send_attack_message(int p_side, int) {
	game_.play_commander_message(p_side, "attack_position");
}

void BFCommander::send_defend_message(int p_side, int) {
	game_.play_commander_message(p_side, "defend");
}

void BFCommander::fire_artillery(int p_side, int p_squad, const Vec2 &p_position) {
	game_.post_request(CommanderRequest::Artillery, p_side, p_position);
	if (p_squad != kNoRequestingSquad) {
		game_.clear_request(p_side, p_squad, CommanderRequest::Artillery);
	}
}

void BFCommander::send_uav(int p_side, int p_squad, const Vec2 &p_position) {
	game_.post_request(CommanderRequest::Uav, p_side, p_position);
	game_.clear_request(p_side, p_squad, CommanderRequest::Uav);
}

void BFCommander::drop_supply(int p_side, int p_squad, const Vec2 &p_position) {
	game_.post_request(CommanderRequest::Supply, p_side, p_position);
	game_.clear_request(p_side, p_squad, CommanderRequest::Supply);
}

void BFCommander::drop_vehicle(int p_side, int p_squad, const Vec2 &p_position) {
	game_.post_request(CommanderRequest::VehicleDrop, p_side, p_position);
	game_.clear_request(p_side, p_squad, CommanderRequest::VehicleDrop);
}

void BFCommander::deny_artillery_request(int p_side, int p_squad) {
	game_.play_commander_message(p_side, "artillery_deny");
	game_.clear_request(p_side, p_squad, CommanderRequest::Artillery);
}

bool BFCommander::game_has_artillery(int p_side) const {
	return game_.has_artillery_pieces(p_side);
}

bool BFCommander::any_artillery_pieces_left(int p_side) const {
	return game_.have_any_artillery(p_side);
}

void BFCommander::artillery_unavailable(int p_side, int p_squad) {
	game_.play_commander_message(p_side, "artillery_unavailable");
	game_.clear_request(p_side, p_squad, CommanderRequest::Artillery);
}

void BFCommander::deny_supply_request(int p_side, int p_squad) {
	game_.play_commander_message(p_side, "supply_deny");
	game_.clear_request(p_side, p_squad, CommanderRequest::Supply);
}

void BFCommander::deny_vehicle_drop_request(int p_side, int p_squad) {
	game_.play_commander_message(p_side, "vehicledrop_deny");
	game_.clear_request(p_side, p_squad, CommanderRequest::VehicleDrop);
}

void BFCommander::deny_uav_request(int p_side, int p_squad) {
	game_.play_commander_message(p_side, "uav_deny");
	game_.clear_request(p_side, p_squad, CommanderRequest::Uav);
}

void CommanderInterface::give_attack_order(const Vec3 &p_position, int p_side, int p_squad) const {
	if (commander_ != nullptr) {
		commander_->give_attack_order(p_position, p_squad, p_side, kNoCommanderTarget);
	}
}

void CommanderInterface::give_move_order(const Vec3 &p_position, int p_side, int p_squad) const {
	if (commander_ != nullptr) {
		commander_->give_move_order(p_position, p_squad, p_side, kNoCommanderTarget);
	}
}

void CommanderInterface::give_defend_order(const Vec3 &p_position, int p_side, int p_squad) const {
	if (commander_ != nullptr) {
		commander_->give_defend_order(p_position, p_squad, p_side, kNoCommanderTarget);
	}
}

}
