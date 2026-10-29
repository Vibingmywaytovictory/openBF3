#pragma once

#include "bf3/ai/squad/squad_world.h"

#include <utility>
#include <vector>

namespace bf3::ai {

class SquadCoordinator;
class SquadMember;

class SquadFormation {
public:
	struct Slot {
		bool occupied = false;
		int member_index = -1;
		Vec3 offset{};
		int trail_index = -1;
		int reference_slot = 0;
	};

	enum class Formation : uint8_t { Line = 0 };

	SquadFormation(SquadCoordinator &r_coordinator, int p_max_members, const FormationTuning &p_tuning);

	void update(sim::Tick p_tick);
	void update_formation();
	void update_wait_for_squad_members(sim::Tick p_tick);
	void update_formation_forward_vector();
	void update_trailing_positions();
	void update_trailing_slots();
	void update_trailing_mode();
	void select_formation();
	int squad_member_index_at_slot(int p_slot) const;
	bool should_leave_squad_leader() const { return leave_squad_leader_; }
	bool should_squad_move_out() const { return move_out_; }
	bool has_formation_moved() const { return moved_; }
	void join_squad_leader();
	float formation_max_speed() const { return formation_max_speed_; }
	void update_formation_max_speed();
	void set_formation();
	void set_line_formation();

	void join_formation(int p_member_index);
	bool is_in_formation(int p_member_index) const;
	void leave_formation(int p_member_index);
	void reset_slot(int p_slot);
	void update_closest_slots();
	int closest_slot_index(int p_slot) const;
	void swap_slots(int p_first, int p_second);
	void update_movement_flag(bool p_force);
	void update_slot_zero_pos();
	void reset_trailing_positions();
	void reindex_member(int p_old_index, int p_new_index);

	Vec3 slot_position(int p_slot) const;
	bool slot_world_position(int p_member_index, Vec3 &r_out) const;
	int slot_of_member(int p_member_index) const;

	int max_members() const { return int(slots_.size()); }
	const Slot &slot(int p_index) const { return slots_[size_t(p_index)]; }
	const Vec3 &direction() const { return direction_; }
	Vec2 forward() const { return forward_; }
	const Vec3 &slot_zero_position() const { return slot_zero_position_; }
	const Vec3 &trailing_position() const { return trailing_position_; }
	const std::pair<bool, Vec3> &slot_target(int p_index) const { return slot_targets_[size_t(p_index)]; }
	bool trailing() const;

private:
	sim::Tick now() const;
	SquadMember *member_at(int p_slot) const;

	SquadCoordinator *coordinator_ = nullptr;
	FormationTuning tuning_;
	std::vector<Slot> slots_;
	Formation formation_ = Formation::Line;
	Vec3 direction_{};
	bool move_out_ = false;
	sim::Tick wait_started_ = 0;
	bool waiting_ = false;
	float formation_max_speed_ = 0.0f;
	int side_ = 0;
	Vec2 forward_{ 0.0f, 1.0f };
	bool moved_ = false;
	Vec3 movement_anchor_{};
	Vec3 slot_zero_position_{};
	bool leave_squad_leader_ = false;
	bool joined_squad_leader_ = false;
	int cycle_ = 0;
	sim::Tick next_movement_stamp_ = 0;
	std::vector<std::pair<bool, Vec3>> slot_targets_;
	Vec3 trailing_position_{};
};

}
