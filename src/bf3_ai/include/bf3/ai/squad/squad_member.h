#pragma once

#include "bf3/ai/squad/squad_world.h"

namespace bf3::ai {

class SquadCoordinator;

class SquadMember {
public:
	SquadMember(SquadCoordinator &r_squad, SquadPlayer &r_player, SquadBot *p_bot, int p_squad_index, int p_side,
			int p_member_index);
	~SquadMember();
	SquadMember(const SquadMember &) = delete;
	SquadMember &operator=(const SquadMember &) = delete;

	SquadPlayer &player() const { return *player_; }
	SquadCoordinator &squad() const { return *squad_; }
	int player_id() const { return player_id_; }
	bool is_bot() const { return bot_ != nullptr; }
	SquadBot *bot() const { return bot_; }
	int member_index() const { return member_index_; }
	void set_member_index(int p_index) { member_index_ = p_index; }
	void detach_bot() { bot_ = nullptr; }
	int squad_index() const { return squad_index_; }
	int side() const { return side_; }
	void set_is_squad_leader(bool p_leader) { is_squad_leader_ = p_leader; }
	bool is_squad_leader() const { return is_squad_leader_; }
	bool is_moving() const { return moved_; }
	void set_have_path(bool p_have_path);
	bool have_path() const { return have_path_; }
	bool needs_path() const { return needs_path_; }
	float formation_distance_sq() const { return formation_distance_sq_; }
	const SquadGoal &goal() const { return goal_; }

	void update(sim::Tick p_tick);
	void update_formation_status();
	bool position(Vec3 &r_position) const;
	bool is_allowed_in_formation() const;
	void update_member_pos();
	bool is_mobile() const;
	void move_with_formation();
	void move_without_formation();
	float vehicle_radius() const;

private:
	void hand_goal(const SquadGoal &p_goal);

	int squad_index_ = 0;
	int member_index_ = 0;
	bool is_squad_leader_ = false;
	int player_id_ = -1;
	SquadPlayer *player_ = nullptr;
	SquadBot *bot_ = nullptr;
	SquadCoordinator *squad_ = nullptr;
	int side_ = 0;
	sim::Tick path_check_tick_ = 0;
	bool have_path_ = false;
	bool needs_path_ = false;
	Vec3 position_{};
	bool position_valid_ = false;
	float formation_distance_sq_ = 900.0f;
	bool moved_ = false;
	Vec3 path_goal_{};
	SquadGoal goal_;
};

}
