#include "bf3/ai/squad/squad_member.h"

#include "bf3/ai/squad/squad_coordinator.h"
#include "bf3/ai/squad/squad_formation.h"

namespace bf3::ai {

namespace {

constexpr sim::Tick kPathCheckTicks = sim::seconds_to_ticks(9.0f);
constexpr float kGoalDisplacedSq = 0.01f;

}

SquadMember::SquadMember(SquadCoordinator &r_squad, SquadPlayer &r_player, SquadBot *p_bot, int p_squad_index,
		int p_side, int p_member_index) :
		squad_index_(p_squad_index), member_index_(p_member_index), player_id_(r_player.id()), player_(&r_player),
		bot_(p_bot), squad_(&r_squad), side_(p_side) {
	if (bot_ != nullptr) {
		bot_->set_squad_member(this);
	}
}

SquadMember::~SquadMember() {
	if (bot_ != nullptr) {
		bot_->set_squad_member(nullptr);
	}
}

void SquadMember::set_have_path(bool p_have_path) {
	have_path_ = p_have_path;
	needs_path_ = !p_have_path;
}

void SquadMember::update(sim::Tick p_tick) {
	update_member_pos();
	if (p_tick > path_check_tick_ + kPathCheckTicks) {
		if (distance_sq(path_goal_, goal_.position) > kGoalDisplacedSq) {
			set_have_path(false);
			path_goal_ = goal_.position;
		}
		path_check_tick_ = p_tick;
	}
	if (squad_->formation().is_in_formation(member_index_)) {
		move_with_formation();
	} else {
		move_without_formation();
	}
}

void SquadMember::update_formation_status() {
	SquadFormation &formation = squad_->formation();
	const bool allowed = is_allowed_in_formation();
	if (formation.is_in_formation(member_index_) && !allowed) {
		formation.leave_formation(member_index_);
		have_path_ = false;
	}
	if (!formation.is_in_formation(member_index_) && allowed) {
		formation.join_formation(member_index_);
		have_path_ = false;
	}
}

bool SquadMember::position(Vec3 &r_position) const {
	r_position = position_;
	return position_valid_;
}

bool SquadMember::is_allowed_in_formation() const {
	if (!player_->is_alive() || player_->is_man_down()) {
		return false;
	}
	const AiObject *const vehicle = player_->current_vehicle();
	return vehicle != nullptr && vehicle == player_->soldier();
}

void SquadMember::update_member_pos() {
	const AiObject *const vehicle = player_->current_vehicle();
	position_valid_ = false;
	moved_ = false;
	if (vehicle == nullptr) {
		return;
	}
	position_ = vehicle->position();
	position_valid_ = true;
	const AiObjectMobile *const mobile = vehicle->mobile();
	if (mobile != nullptr) {
		const Vec3 &speed = mobile->speed();
		const float moving = squad_->services().formation.moving_speed_mps;
		moved_ = speed.x * speed.x + speed.z * speed.z > moving * moving;
	}
}

bool SquadMember::is_mobile() const {
	const AiObject *const vehicle = player_->current_vehicle();
	if (vehicle == nullptr || !player_->is_alive() || player_->is_man_down()) {
		return false;
	}
	return vehicle->mobile() != nullptr;
}

void SquadMember::hand_goal(const SquadGoal &p_goal) {
	goal_ = p_goal;
	if (bot_ != nullptr) {
		bot_->set_squad_goal(p_goal);
	}
}

void SquadMember::move_with_formation() {
	const SquadFormation &formation = squad_->formation();
	SquadGoal goal;
	if (formation.squad_member_index_at_slot(0) == member_index_) {
		if (squad_->mission_status() == SquadMissionStatus::Accepted) {
			goal.kind = SquadGoalKind::SquadTarget;
			goal.position = squad_->squad_goto_position();
		}
		goal.max_speed = formation.formation_max_speed();
		goal.wait_for_members = !formation.should_squad_move_out();
	} else if (formation.slot_world_position(member_index_, goal.position)) {
		goal.kind = SquadGoalKind::Slot;
	}
	hand_goal(goal);
}

void SquadMember::move_without_formation() {
	if (bot_ == nullptr || !player_->is_alive()) {
		return;
	}
	SquadGoal goal;
	if (squad_->mission_status() == SquadMissionStatus::Accepted) {
		goal.kind = SquadGoalKind::SquadTarget;
		goal.position = squad_->squad_goto_position();
	}
	hand_goal(goal);
}

float SquadMember::vehicle_radius() const {
	const AiObject *const vehicle = player_->current_vehicle();
	const AiObjectPhysical *const physical = vehicle != nullptr ? vehicle->physical() : nullptr;
	return physical != nullptr ? physical->radius() : 0.0f;
}

}
