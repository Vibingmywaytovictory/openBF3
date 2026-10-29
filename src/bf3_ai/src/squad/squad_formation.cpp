#include "bf3/ai/squad/squad_formation.h"

#include "bf3/ai/squad/squad_coordinator.h"
#include "bf3/ai/squad/squad_member.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <list>

namespace bf3::ai {

namespace {

constexpr float kLeaderRangeSq = 2500.0f;
constexpr float kTrailPointSetSq = 0.001f;
constexpr float kTrailCrumbSq = 36.0f;
constexpr float kMovementSq = 4.0f;
constexpr sim::Tick kMovementStampTicks = sim::seconds_to_ticks(0.2f);
constexpr float kIdleFormationSpeed = 5.0f;
constexpr float kCrowdedSpeedFactor = 0.89999998f;

struct SquadMemberDistanceObj {
	SquadMember *member = nullptr;
	float distance_sq = 0.0f;
	bool operator<(const SquadMemberDistanceObj &p_other) const { return distance_sq < p_other.distance_sq; }
};

bool trail_point_is_set(const Vec3 &p_point) {
	return p_point.x * p_point.x + p_point.y * p_point.y + p_point.z * p_point.z > kTrailPointSetSq;
}

}

SquadFormation::SquadFormation(SquadCoordinator &r_coordinator, int p_max_members, const FormationTuning &p_tuning) :
		coordinator_(&r_coordinator), tuning_(p_tuning) {
	side_ = r_coordinator.side();
	const size_t count = size_t(std::max(p_max_members, 1));
	slots_.resize(count);
	slot_targets_.assign(count, { false, Vec3{} });
	formation_max_speed_ = FLT_MAX;
	update_formation_max_speed();
	formation_ = Formation::Line;
	set_formation();
}

sim::Tick SquadFormation::now() const {
	const SquadWorld *const world = coordinator_->services().world;
	return world != nullptr ? world->game_tick() : 0;
}

SquadMember *SquadFormation::member_at(int p_slot) const {
	const Slot &slot = slots_[size_t(p_slot)];
	if (!slot.occupied || slot.member_index < 0 || slot.member_index >= coordinator_->number_of_squad_members()) {
		return nullptr;
	}
	return coordinator_->squad_member(slot.member_index);
}

void SquadFormation::update(sim::Tick p_tick) {
	update_slot_zero_pos();
	update_movement_flag(false);
	update_formation_max_speed();
	update_formation();
	update_wait_for_squad_members(p_tick);
	update_formation_forward_vector();
	update_trailing_positions();
	update_trailing_mode();
	update_trailing_slots();
	if (++cycle_ > 5) {
		cycle_ = 0;
		set_formation();
	} else if (cycle_ == 3) {
		select_formation();
	}
}

void SquadFormation::select_formation() {
	formation_ = Formation::Line;
}

void SquadFormation::update_formation_forward_vector() {
	const SquadMember *const anchor = member_at(0);
	if (anchor == nullptr) {
		return;
	}
	const AiObject *const vehicle = anchor->player().current_vehicle();
	const AiObjectMobile *const mobile = vehicle != nullptr ? vehicle->mobile() : nullptr;
	if (mobile == nullptr) {
		return;
	}
	const Vec3 &speed = mobile->speed();
	const float length = std::sqrt(speed.x * speed.x + speed.z * speed.z);
	if (length > tuning_.moving_speed_mps) {
		forward_ = { speed.x / length, speed.z / length };
	}
}

void SquadFormation::update_trailing_positions() {
	if (!trail_point_is_set(slot_zero_position_)) {
		return;
	}
	if (!trail_point_is_set(trailing_position_)) {
		trailing_position_ = slot_zero_position_;
		return;
	}
	if (!slots_[0].occupied) {
		return;
	}
	if (!(distance_sq(slot_zero_position_, trailing_position_) > kTrailCrumbSq)) {
		return;
	}
	for (size_t index = slot_targets_.size() - 1; index != 0; --index) {
		slot_targets_[index] = slot_targets_[index - 1];
	}
	slot_targets_[0] = { true, trailing_position_ };
	trailing_position_ = slot_zero_position_;
}

bool SquadFormation::trailing() const {
	return std::any_of(slots_.begin(), slots_.end(), [](const Slot &p_slot) { return p_slot.trail_index != -1; });
}

void SquadFormation::update_trailing_mode() {
	if (!tuning_.trail_while_moving) {
		return;
	}
	const SquadMember *const anchor = member_at(0);
	const bool moving = anchor != nullptr && anchor->is_moving();
	if (moving && !trailing()) {
		int trail_index = 0;
		for (size_t index = 1; index < slots_.size(); ++index) {
			if (slots_[index].occupied) {
				slots_[index].trail_index = trail_index++;
			}
		}
	} else if (!moving && trailing()) {
		reset_trailing_positions();
	}
}

void SquadFormation::update_trailing_slots() {
	std::list<SquadMemberDistanceObj> members;
	for (size_t index = 0; index < slots_.size(); ++index) {
		const Slot &slot = slots_[index];
		SquadMember *const member = slot.trail_index != -1 ? member_at(int(index)) : nullptr;
		if (member != nullptr) {
			Vec3 position{};
			member->position(position);
			members.push_back({ member, distance_sq(position, slot_zero_position_) });
		}
	}
	if (members.empty()) {
		return;
	}
	members.sort();
	for (Slot &slot : slots_) {
		slot.trail_index = -1;
	}
	int trail_index = 0;
	for (const SquadMemberDistanceObj &entry : members) {
		for (size_t index = 1; index < slots_.size(); ++index) {
			if (slots_[index].member_index == entry.member->member_index()) {
				slots_[index].trail_index = trail_index;
				break;
			}
		}
		++trail_index;
	}
}

int SquadFormation::squad_member_index_at_slot(int p_slot) const {
	return slots_[size_t(p_slot)].member_index;
}

void SquadFormation::join_squad_leader() {
	joined_squad_leader_ = true;
	leave_squad_leader_ = false;
	update_movement_flag(true);
}

void SquadFormation::update_formation() {
	SquadMember *const leader = coordinator_->squad_leader();
	if (leader == nullptr) {
		return;
	}
	const int leader_index = leader->member_index();
	const bool leader_is_bot = leader->is_bot();
	Vec3 squad_position{};
	coordinator_->squad_position(squad_position);

	size_t leader_slot = 0;
	bool leader_present = false;
	for (; leader_slot < slots_.size(); ++leader_slot) {
		if (slots_[leader_slot].occupied && slots_[leader_slot].member_index == leader_index) {
			leader_present = true;
			break;
		}
	}

	if (leader_present) {
		if (leave_squad_leader_) {
			if (leader_slot != 0) {
				return;
			}
			for (size_t index = slots_.size() - 1; index > 0; --index) {
				Slot &other = slots_[index];
				if (!other.occupied) {
					continue;
				}
				std::swap(slots_[0].member_index, other.member_index);
				reset_trailing_positions();
				coordinator_->squad_member(slots_[0].member_index)->set_have_path(false);
				coordinator_->squad_member(other.member_index)->set_have_path(false);
				return;
			}
			return;
		}
		if (leader_slot == 0) {
			return;
		}
		Vec3 leader_position{};
		leader->position(leader_position);
		if (!(distance_sq(leader_position, squad_position) < kLeaderRangeSq) && !joined_squad_leader_) {
			return;
		}
		joined_squad_leader_ = false;
		Slot &zero = slots_[0];
		const bool old_occupied = zero.occupied;
		const int old_index = zero.member_index;
		zero.occupied = true;
		zero.member_index = leader_index;
		coordinator_->squad_member(leader_index)->set_have_path(false);
		Slot &former = slots_[leader_slot];
		former.occupied = old_occupied;
		former.member_index = old_index;
		if (old_occupied) {
			coordinator_->squad_member(old_index)->set_have_path(false);
		}
		reset_trailing_positions();
		return;
	}

	Slot &zero = slots_[0];
	if (!zero.occupied || coordinator_->squad_member(zero.member_index)->is_bot() == leader_is_bot) {
		return;
	}
	for (size_t index = 1; index < slots_.size(); ++index) {
		Slot &other = slots_[index];
		if (!other.occupied || coordinator_->squad_member(other.member_index)->is_bot() != leader_is_bot) {
			continue;
		}
		std::swap(zero.member_index, other.member_index);
		coordinator_->squad_member(zero.member_index)->set_have_path(false);
		reset_trailing_positions();
		return;
	}
}

void SquadFormation::update_wait_for_squad_members(sim::Tick p_tick) {
	Vec3 squad_position{};
	coordinator_->squad_position(squad_position);
	bool all_close = true;
	for (size_t index = 1; index < slots_.size(); ++index) {
		const SquadMember *const member = member_at(int(index));
		Vec3 position{};
		if (member != nullptr && member->position(position) &&
				distance_sq(position, squad_position) > member->formation_distance_sq()) {
			all_close = false;
		}
	}
	if (all_close) {
		waiting_ = false;
		move_out_ = true;
		return;
	}
	if (!waiting_) {
		waiting_ = true;
		wait_started_ = p_tick;
	}
	move_out_ = p_tick >= wait_started_ + sim::seconds_to_ticks(tuning_.max_wait_s);
}

bool SquadFormation::is_in_formation(int p_member_index) const {
	return std::any_of(slots_.begin(), slots_.end(),
			[p_member_index](const Slot &p_slot) { return p_slot.member_index == p_member_index; });
}

int SquadFormation::slot_of_member(int p_member_index) const {
	for (size_t index = 0; index < slots_.size(); ++index) {
		if (slots_[index].occupied && slots_[index].member_index == p_member_index) {
			return int(index);
		}
	}
	return -1;
}

void SquadFormation::reset_slot(int p_slot) {
	Slot &slot = slots_[size_t(p_slot)];
	slot.occupied = false;
	slot.member_index = -1;
	slot.trail_index = -1;
}

void SquadFormation::leave_formation(int p_member_index) {
	const size_t count = slots_.size();
	size_t removed = 0;
	for (; removed < count; ++removed) {
		if (slots_[removed].member_index == p_member_index) {
			reset_slot(int(removed));
			break;
		}
	}

	auto move_slot = [this](size_t p_to, size_t p_from) {
		Slot &destination = slots_[p_to];
		const Slot &source = slots_[p_from];
		destination.occupied = source.occupied;
		destination.offset = source.offset;
		destination.member_index = source.member_index;
		destination.trail_index = source.trail_index;
		coordinator_->squad_member(destination.member_index)->set_have_path(false);
		reset_slot(int(p_from));
	};

	if (removed + 1 < count && slots_[removed + 1].occupied) {
		for (size_t last = count - 1; last > 0; --last) {
			if (!slots_[last].occupied) {
				continue;
			}
			if (removed == 0 && last > 1) {
				size_t selected = last;
				float minimum = FLT_MAX;
				const Vec3 &destination = coordinator_->squad_goto_position();
				for (size_t candidate = last; candidate > 0; --candidate) {
					const SquadMember *const member = member_at(int(candidate));
					Vec3 position{};
					if (member != nullptr && !member->is_squad_leader() && member->is_bot() &&
							member->position(position)) {
						const float distance = distance_sq(position, destination);
						if (distance < minimum) {
							minimum = distance;
							selected = candidate;
						}
					}
				}
				move_slot(0, selected);
				if (selected + 1 < count && slots_[selected + 1].occupied) {
					for (size_t replacement = count - 1; replacement > 0; --replacement) {
						if (slots_[replacement].occupied) {
							move_slot(selected, replacement);
							break;
						}
					}
				}
			} else {
				const int moved_member = slots_[last].member_index;
				const int trail_index = slots_[last].trail_index;
				reset_slot(int(last));
				Slot &slot = slots_[removed];
				slot.occupied = true;
				slot.member_index = moved_member;
				slot.trail_index = trail_index;
				coordinator_->squad_member(moved_member)->set_have_path(false);
			}
			break;
		}
	}
	if (removed == 0) {
		reset_trailing_positions();
	}
	update_movement_flag(true);
}

void SquadFormation::update_closest_slots() {
	for (size_t slot = 1; slot < slots_.size(); ++slot) {
		if (slots_[slot].occupied) {
			const int closest = closest_slot_index(int(slot));
			if (closest != -1 && closest != int(slot)) {
				swap_slots(int(slot), closest);
			}
		}
	}
}

int SquadFormation::closest_slot_index(int p_slot) const {
	const SquadMember *const member = member_at(p_slot);
	Vec3 position{};
	if (member == nullptr || !member->position(position) || !trail_point_is_set(slot_zero_position_) || trailing()) {
		return -1;
	}
	int best = -1;
	float minimum = FLT_MAX;
	for (size_t index = 1; index < slots_.size(); ++index) {
		if (!slots_[index].occupied) {
			continue;
		}
		const float distance = distance_sq(position, slot_position(int(index)));
		if (distance < minimum) {
			minimum = distance;
			best = int(index);
		}
	}
	return best;
}

void SquadFormation::swap_slots(int p_first, int p_second) {
	std::swap(slots_[size_t(p_first)].member_index, slots_[size_t(p_second)].member_index);
}

void SquadFormation::join_formation(int p_member_index) {
	for (Slot &slot : slots_) {
		if (!slot.occupied) {
			slot.occupied = true;
			slot.member_index = p_member_index;
			slot.trail_index = -1;
			break;
		}
	}
	update_movement_flag(true);
	coordinator_->squad_member(p_member_index)->set_have_path(false);
}

void SquadFormation::update_movement_flag(bool p_force) {
	if (!slots_[0].occupied) {
		return;
	}
	moved_ = false;
	const sim::Tick tick = now();
	const bool update = p_force ||
			(distance_sq(movement_anchor_, slot_zero_position_) > kMovementSq && tick > next_movement_stamp_);
	if (update) {
		moved_ = true;
		next_movement_stamp_ = tick + kMovementStampTicks;
		movement_anchor_ = slot_zero_position_;
	}
}

void SquadFormation::update_slot_zero_pos() {
	const SquadMember *const anchor = member_at(0);
	Vec3 position{};
	if (anchor != nullptr && coordinator_->squad_has_position() && anchor->position(position)) {
		slot_zero_position_ = position;
	}
}

void SquadFormation::reset_trailing_positions() {
	update_slot_zero_pos();
	trailing_position_ = slot_zero_position_;
	for (size_t index = 0; index < slots_.size(); ++index) {
		slot_targets_[index].first = false;
		slots_[index].trail_index = -1;
	}
}

void SquadFormation::reindex_member(int p_old_index, int p_new_index) {
	for (Slot &slot : slots_) {
		if (slot.member_index == p_old_index) {
			slot.member_index = p_new_index;
		}
	}
}

void SquadFormation::update_formation_max_speed() {
	int occupied = 0;
	formation_max_speed_ = FLT_MAX;
	for (size_t index = 0; index < slots_.size(); ++index) {
		if (!slots_[index].occupied) {
			continue;
		}
		++occupied;
		const SquadMember *const member = member_at(int(index));
		const AiObject *const vehicle = member != nullptr ? member->player().current_vehicle() : nullptr;
		const AiObjectMobile *const mobile = vehicle != nullptr ? vehicle->mobile() : nullptr;
		if (mobile != nullptr) {
			formation_max_speed_ = std::min(formation_max_speed_, mobile->max_speed());
		}
	}
	if (occupied == 0) {
		formation_max_speed_ = kIdleFormationSpeed;
	} else if (occupied > 1) {
		formation_max_speed_ = kCrowdedSpeedFactor * formation_max_speed_;
	}
}

void SquadFormation::set_formation() {
	if (formation_ == Formation::Line) {
		direction_ = { 1.0f, 0.0f, 0.0f };
		set_line_formation();
	}
}

void SquadFormation::set_line_formation() {
	bool positive_side = true;
	int reference_slot = 0;
	for (size_t index = 0; index < slots_.size(); ++index) {
		Slot &slot = slots_[index];
		float spacing = tuning_.line_spacing_m;
		const SquadMember *const member = member_at(int(index));
		if (member != nullptr) {
			spacing = std::max(spacing, 2.0f * member->vehicle_radius());
		}
		float offset = 0.0f;
		if (index != 0) {
			if (positive_side) {
				offset = spacing * direction_.x;
				slot.reference_slot = reference_slot;
				positive_side = false;
				if (index >= 3) {
					++reference_slot;
				}
			} else {
				offset = -spacing * direction_.x;
				slot.reference_slot = reference_slot++;
				positive_side = true;
			}
		}
		slot.offset = { offset, 0.0f, 0.0f };
	}
}

Vec3 SquadFormation::slot_position(int p_slot) const {
	if (p_slot <= 0) {
		return slot_zero_position_;
	}
	const Slot &slot = slots_[size_t(p_slot)];
	const Vec3 base = slot_position(std::min(slot.reference_slot, p_slot - 1));
	const float right_x = forward_.y;
	const float right_z = -forward_.x;
	return { base.x + right_x * slot.offset.x + forward_.x * slot.offset.z, base.y,
		base.z + right_z * slot.offset.x + forward_.y * slot.offset.z };
}

bool SquadFormation::slot_world_position(int p_member_index, Vec3 &r_out) const {
	const int slot = slot_of_member(p_member_index);
	if (slot == -1) {
		return false;
	}
	const int trail_index = slots_[size_t(slot)].trail_index;
	if (slot != 0 && trail_index >= 0 && size_t(trail_index) < slot_targets_.size() &&
			slot_targets_[size_t(trail_index)].first) {
		r_out = slot_targets_[size_t(trail_index)].second;
		return true;
	}
	r_out = slot_position(slot);
	return true;
}

}
