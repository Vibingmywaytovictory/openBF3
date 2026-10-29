#include "bf3/ai/squad/squad_coordinator.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"

#include <algorithm>

namespace bf3::ai {

namespace {

constexpr float kShooterMovedSq = 25.0f;
constexpr float kLeaderNearGotoSq = 225.0f;
constexpr float kNoGotoDistanceSq = 1000.0f;
constexpr float kSquadPositionSetSq = 0.001f;

}

SquadCoordinator::SquadCoordinator(const SquadServices &p_services, int p_side, int p_squad_index) :
		services_(p_services), squad_index_(p_squad_index), side_(p_side) {
	const int members = services_.world != nullptr ? services_.world->max_number_of_squad_members() : 0;
	formation_ = std::make_unique<SquadFormation>(*this, members, services_.formation);
	reset_need_order_message_timer(services_.world != nullptr ? services_.world->game_tick() : 0);
}

void SquadCoordinator::update(sim::Tick p_tick) {
	now_ = p_tick;
	const SquadStrategy *const strategy = services_.strategy;
	if (mission_status_ == SquadMissionStatus::Accepted && strategy != nullptr &&
			!strategy->is_player_commander(side_ + 1)) {
		if (order_ == SquadOrder::Attack) {
			if (area_ != nullptr && area_->status(side_ + 1) != 0) {
				if (!area_->is_takeable() && !strategy->own_all_takeable_control_points(side_ + 1)) {
					un_assign_squad();
				}
			} else if (area_ != nullptr && area_->status(side_ + 1) == 0) {
				un_assign_squad();
			}
		} else if (order_ == SquadOrder::Defend && area_ != nullptr && area_->status(side_ + 1) == 0) {
			const int other_side = (side_ == 0) + 1;
			if (strategy->have_any_owned_and_takeable_control_points(other_side)) {
				un_assign_squad();
			}
		}
	}

	if (members_.empty()) {
		return;
	}
	for (size_t index = 0; index < members_.size(); ++index) {
		members_[index]->update_formation_status();
	}
	formation_->update_closest_slots();
	for (size_t index = 0; index < members_.size(); ++index) {
		members_[index]->update(p_tick);
	}
	formation_->update(p_tick);
	update_squad_position();
	update_shots_fired();
	update_repair_request(p_tick);
	update_need_orders_message(p_tick);
}

void SquadCoordinator::update_squad_position() {
	const int anchor_index = formation_->squad_member_index_at_slot(0);
	SquadMember *member = anchor_index >= 0 && anchor_index < number_of_squad_members()
			? members_[size_t(anchor_index)].get()
			: squad_leader();
	if (member == nullptr || !member->player().is_alive()) {
		member = nullptr;
		for (const std::unique_ptr<SquadMember> &candidate : members_) {
			if (candidate->player().is_alive()) {
				member = candidate.get();
				break;
			}
		}
	}
	if (member != nullptr) {
		member->position(squad_position_);
	}
}

void SquadCoordinator::shots_fired(int p_player_id) {
	int index = 0;
	const SquadMember *const member = squad_member(p_player_id, index);
	if (member == nullptr) {
		return;
	}
	const sim::Tick expires = now_ + kSquadFireRecordTicks;
	for (SquadMemberFire &fire : fires_) {
		if (fire.player_id == p_player_id) {
			fire.expires = expires;
			return;
		}
	}
	SquadMemberFire fire;
	fire.expires = expires;
	fire.player_id = p_player_id;
	member->position(fire.position);
	fires_.push_back(fire);
}

void SquadCoordinator::update_shots_fired() {
	for (auto it = fires_.begin(); it != fires_.end();) {
		int index = 0;
		const SquadMember *const member = squad_member(it->player_id, index);
		Vec3 position{};
		if (member == nullptr || !member->position(position)) {
			it = fires_.erase(it);
		} else if (now_ > it->expires || distance_sq(it->position, position) > kShooterMovedSq) {
			it = fires_.erase(it);
			for (const std::unique_ptr<SquadMember> &each : members_) {
				each->set_have_path(false);
			}
			formation_->update_movement_flag(true);
		} else {
			++it;
		}
	}
}

void SquadCoordinator::request_repair(const Vec3 &p_position, sim::Tick p_tick) {
	repair_requested_ = true;
	repair_confirmed_ = false;
	repair_position_ = p_position;
	repair_deadline_ = p_tick + kSquadRepairRequestTicks;
}

void SquadCoordinator::update_repair_request(sim::Tick p_tick) {
	if (repair_requested_ && p_tick > repair_deadline_) {
		repair_requested_ = false;
		repair_confirmed_ = false;
	}
}

void SquadCoordinator::update_need_orders_message(sim::Tick p_tick) {
	const SquadStrategy *const strategy = services_.strategy;
	if (strategy != nullptr && strategy->is_player_commander(side_ + 1) && !is_squad_taking_flag() &&
			(order_ == SquadOrder::Attack || order_ == SquadOrder::Move || order_ == SquadOrder::None)) {
		const SquadMember *const leader = squad_leader();
		if (leader != nullptr) {
			float distance = kNoGotoDistanceSq;
			Vec3 position{};
			if (leader->position(position)) {
				distance = distance_sq(position, goto_position_);
			}
			if (leader->is_bot() && !leader->is_moving() && leader->player().is_alive() &&
					!leader->player().is_man_down() &&
					(distance < kLeaderNearGotoSq || (area_ != nullptr && area_->status(side_ + 1) == 0))) {
				if (!(p_tick > need_order_message_tick_ + kSquadNeedOrdersTicks)) {
					return;
				}
				if (services_.squads != nullptr) {
					services_.squads->request_new_commander_order(squad_index_ + 1, leader->player());
				}
			}
		}
	}
	reset_need_order_message_timer(p_tick);
}

bool SquadCoordinator::is_squad_taking_flag() const {
	if (area_ == nullptr || area_->status(side_ + 1) == 0) {
		return false;
	}
	Vec3 control_point{};
	float radius = 0.0f;
	if (!area_->control_point(control_point, radius)) {
		return false;
	}
	const float radius_sq = radius * radius;
	for (const std::unique_ptr<SquadMember> &member : members_) {
		Vec3 position{};
		if (member->player().is_alive() && !member->player().is_man_down() && member->position(position) &&
				distance_sq(control_point, position) < radius_sq) {
			return true;
		}
	}
	return false;
}

void SquadCoordinator::update_squad_paths() {
	for (const std::unique_ptr<SquadMember> &member : members_) {
		member->set_have_path(false);
	}
}

bool SquadCoordinator::is_squad_assigned_to_area(int p_area_index) const {
	return area_ != nullptr && area_->index() == p_area_index;
}

int SquadCoordinator::strategic_strength(int p_type) const {
	float total = 0.0f;
	if (p_type < 0 || p_type >= kStrategicStrengthCount || services_.information == nullptr) {
		return 0;
	}
	for (const std::unique_ptr<SquadMember> &member : members_) {
		const SquadPlayer &player = member->player();
		if (!player.is_alive() || player.is_man_down()) {
			continue;
		}
		const AiObject *const vehicle = player.current_vehicle();
		if (vehicle == nullptr) {
			continue;
		}
		const uint32_t handle = vehicle->information(side_ + 1);
		const Information *const information =
				(handle & kHandleIndexMask) != 0 ? services_.information->information(handle) : nullptr;
		const IPIUnit *const unit = information != nullptr ? information->unit() : nullptr;
		if (unit != nullptr) {
			total += unit->strategic_strengths()[p_type];
		}
	}
	return int(total + 0.5f);
}

SquadMember *SquadCoordinator::squad_leader() const {
	if (leader_index_ < 0 || size_t(leader_index_) >= members_.size()) {
		return nullptr;
	}
	return members_[size_t(leader_index_)].get();
}

bool SquadCoordinator::squad_has_position() const {
	return !(distance_sq(squad_position_, Vec3{}) < kSquadPositionSetSq);
}

void SquadCoordinator::un_assign_squad() {
	area_ = nullptr;
	order_ = SquadOrder::None;
	mission_status_ = SquadMissionStatus::None;
}

void SquadCoordinator::assign_squad(
		const SquadArea *p_area, const Vec3 &p_goto, SquadOrder p_order, SquadMissionStatus p_status) {
	area_ = p_area;
	goto_position_ = p_goto;
	order_ = p_order;
	mission_status_ = p_status;
	update_squad_paths();
}

void SquadCoordinator::add_squad_member(SquadPlayer &r_player) {
	SquadBot *const bot = services_.world != nullptr ? services_.world->bot_from_player_id(r_player.id()) : nullptr;
	auto member = std::make_unique<SquadMember>(*this, r_player, bot, squad_index_, side_, int(members_.size()));
	if (!member->is_bot() && services_.strategy != nullptr && !services_.strategy->is_player_commander(side_ + 1)) {
		un_assign_squad();
	}
	SquadMember &added = *member;
	members_.push_back(std::move(member));
	if (added.is_allowed_in_formation()) {
		formation_->join_formation(added.member_index());
		formation_->update_movement_flag(true);
	}
	added.update_member_pos();
}

bool SquadCoordinator::remove_squad_member(int p_player_id) {
	int index = 0;
	if (squad_member(p_player_id, index) == nullptr) {
		return false;
	}
	if (formation_->is_in_formation(index)) {
		formation_->leave_formation(index);
	}
	fires_.remove_if([p_player_id](const SquadMemberFire &p_fire) { return p_fire.player_id == p_player_id; });
	members_.erase(members_.begin() + index);
	for (size_t moved = size_t(index); moved < members_.size(); ++moved) {
		formation_->reindex_member(int(moved) + 1, int(moved));
		members_[moved]->set_member_index(int(moved));
	}
	const bool leader_left = leader_index_ == index;
	if (leader_left) {
		leader_index_ = -1;
	} else if (leader_index_ > index) {
		--leader_index_;
	}
	update_squad_leader();
	if (leader_left && squad_leader() != nullptr) {
		on_leader_changed();
	}
	return true;
}

void SquadCoordinator::update_squad_leader() {
	const int old_leader = leader_index_;
	bool found = false;
	for (size_t index = 0; index < members_.size(); ++index) {
		SquadMember &member = *members_[index];
		member.set_is_squad_leader(
				services_.squads != nullptr && services_.squads->is_squad_leader(squad_index_ + 1, member.player()));
		if (member.is_squad_leader()) {
			leader_index_ = int(index);
			found = true;
			break;
		}
	}
	if (!found) {
		return;
	}
	if (old_leader != leader_index_ && old_leader != -1 && !members_.empty()) {
		on_leader_changed();
	}
	if (members_.size() == 1) {
		formation_->reset_trailing_positions();
	}
}

void SquadCoordinator::on_leader_changed() {
	members_[size_t(leader_index_)]->update_member_pos();
	formation_->join_squad_leader();
	update_squad_position();
	update_squad_paths();
}

SquadMember *SquadCoordinator::squad_member(int p_player_id, int &r_index) const {
	for (size_t index = 0; index < members_.size(); ++index) {
		if (members_[index]->player_id() == p_player_id) {
			r_index = int(index);
			return members_[index].get();
		}
	}
	return nullptr;
}

bool SquadCoordinator::has_member_with_kit(int p_kit) const {
	return std::any_of(members_.begin(), members_.end(),
			[p_kit](const std::unique_ptr<SquadMember> &p_member) { return p_member->player().spawn_kit() == p_kit; });
}

}
