#include "server_slot_view.h"

#include "bf3/host/kits.h"

#include <algorithm>

namespace bf3::host {

std::span<const sim::SoldierState> SlotView::states() const {
	if (!cached_ || cache_tick_ != tick_) {
		cache_.assign(slots_.size(), sim::SoldierState());
		for (size_t i = 0; i < slots_.size(); ++i) {
			if (slots_[i].used && soldiers_[i].spawned) {
				cache_[i] = soldiers_[i].state;
			}
		}
		cache_tick_ = tick_;
		cached_ = true;
	}
	return cache_;
}

uint8_t SlotView::team(uint8_t p_slot) const {
	return p_slot < slots_.size() && slots_[p_slot].used ? uint8_t(std::clamp(slots_[p_slot].team, 0, 255)) : uint8_t(0);
}

bool SlotView::alive(uint8_t p_slot) const {
	return p_slot < slots_.size() && slots_[p_slot].used && soldiers_.alive(p_slot);
}

const sim::weapons::SoldierWeaponsState &SlotView::weapons(uint8_t p_slot) const {
	static const sim::weapons::SoldierWeaponsState s_empty;
	return combat_.on() ? combat_.gunplay()->weapons(p_slot) : s_empty;
}

std::span<const DownedSoldier> SlotView::downed() const {
	return match_.on() ? match_.rules()->downed() : ServerView::downed();
}

net::MatchPhase SlotView::phase() const {
	return match_.on() ? match_.rules()->phase() : ServerView::phase();
}

bool SlotView::input_restricted() const {
	return match_.on() ? match_.rules()->input_restricted() : ServerView::input_restricted();
}

uint8_t SlotView::kit(uint8_t p_slot) const {
	if (!match_.on()) {
		return ServerView::kit(p_slot);
	}
	const uint8_t k = match_.rules()->kit_of(p_slot);
	return k != kKeepKit ? k : (combat_.on() ? combat_.gunplay()->kit(p_slot) : uint8_t(0));
}

}
