#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <cstdint>
#include <string_view>

namespace bf3::ai {

enum class CommanderOrder : int { Move = 4, Attack = 5, Defend = 6 };
enum class CommanderRequest : int { Artillery = 0, Uav = 1, VehicleDrop = 2, Supply = 3 };

inline constexpr uint16_t kNoCommanderTarget = 0;
inline constexpr int kNoRequestingSquad = -1;

class Commander {
public:
	virtual ~Commander() = default;
	virtual void apply_as_commander(int p_player_id, int p_side) = 0;
	virtual void retire_as_commander(int p_side) = 0;
	virtual void give_attack_order(const Vec3 &p_position, int p_squad, int p_side, uint16_t p_target) = 0;
	virtual void give_move_order(const Vec3 &p_position, int p_squad, int p_side, uint16_t p_target) = 0;
	virtual void give_defend_order(const Vec3 &p_position, int p_squad, int p_side, uint16_t p_target) = 0;
	virtual void send_attack_message(int p_side, int p_squad) = 0;
	virtual void send_defend_message(int p_side, int p_squad) = 0;
	virtual void fire_artillery(int p_side, int p_squad, const Vec2 &p_position) = 0;
	virtual void send_uav(int p_side, int p_squad, const Vec2 &p_position) = 0;
	virtual void drop_supply(int p_side, int p_squad, const Vec2 &p_position) = 0;
	virtual void drop_vehicle(int p_side, int p_squad, const Vec2 &p_position) = 0;
	virtual void deny_artillery_request(int p_side, int p_squad) = 0;
	virtual bool game_has_artillery(int p_side) const = 0;
	virtual bool any_artillery_pieces_left(int p_side) const = 0;
	virtual void artillery_unavailable(int p_side, int p_squad) = 0;
	virtual void deny_supply_request(int p_side, int p_squad) = 0;
	virtual void deny_vehicle_drop_request(int p_side, int p_squad) = 0;
	virtual void deny_uav_request(int p_side, int p_squad) = 0;
};

class CommanderGame {
public:
	virtual ~CommanderGame() = default;
	virtual void apply_for_commander(int p_side, int p_player_id) = 0;
	virtual void resign_commander(int p_side) = 0;
	virtual void issue_commander_order(
			int p_side, int p_squad, CommanderOrder p_order, const Vec3 &p_position, uint16_t p_target) = 0;
	virtual void post_request(CommanderRequest p_request, int p_side, const Vec2 &p_position) = 0;
	virtual void clear_request(int p_side, int p_squad, CommanderRequest p_request) = 0;
	virtual void play_commander_message(int p_side, std::string_view p_name) = 0;
	virtual bool has_artillery_pieces(int p_side) const = 0;
	virtual bool have_any_artillery(int p_side) const = 0;
};

class BFCommander final : public Commander {
public:
	explicit BFCommander(CommanderGame &r_game) : game_(r_game) {}

	void apply_as_commander(int p_player_id, int p_side) override;
	void retire_as_commander(int p_side) override;
	void give_attack_order(const Vec3 &p_position, int p_squad, int p_side, uint16_t p_target) override;
	void give_move_order(const Vec3 &p_position, int p_squad, int p_side, uint16_t p_target) override;
	void give_defend_order(const Vec3 &p_position, int p_squad, int p_side, uint16_t p_target) override;
	void send_attack_message(int p_side, int p_squad) override;
	void send_defend_message(int p_side, int p_squad) override;
	void fire_artillery(int p_side, int p_squad, const Vec2 &p_position) override;
	void send_uav(int p_side, int p_squad, const Vec2 &p_position) override;
	void drop_supply(int p_side, int p_squad, const Vec2 &p_position) override;
	void drop_vehicle(int p_side, int p_squad, const Vec2 &p_position) override;
	void deny_artillery_request(int p_side, int p_squad) override;
	bool game_has_artillery(int p_side) const override;
	bool any_artillery_pieces_left(int p_side) const override;
	void artillery_unavailable(int p_side, int p_squad) override;
	void deny_supply_request(int p_side, int p_squad) override;
	void deny_vehicle_drop_request(int p_side, int p_squad) override;
	void deny_uav_request(int p_side, int p_squad) override;

private:
	CommanderGame &game_;
};

class CommanderInterface {
public:
	explicit CommanderInterface(Commander *p_commander) : commander_(p_commander) {}

	void give_attack_order(const Vec3 &p_position, int p_side, int p_squad) const;
	void give_move_order(const Vec3 &p_position, int p_side, int p_squad) const;
	void give_defend_order(const Vec3 &p_position, int p_side, int p_squad) const;

private:
	Commander *commander_ = nullptr;
};

}
