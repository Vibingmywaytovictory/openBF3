#pragma once

#include "bf3/host/server_view.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_state.h"
#include "server_combat.h"
#include "server_match.h"
#include "server_slots.h"
#include "server_soldiers.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::host {

class SlotView final : public ServerView {
public:
	SlotView(const SlotTable &p_slots, const Soldiers &p_soldiers, const MatchControl &p_match, const Combat &p_combat, const sim::Tick &p_tick) :
			slots_(p_slots), soldiers_(p_soldiers), match_(p_match), combat_(p_combat), tick_(p_tick) {}

	void configure(const sim::CollisionWorld &p_world) { world_ = &p_world; }
	bool ready() const { return world_ != nullptr; }

	std::span<const sim::SoldierState> states() const override;
	uint8_t team(uint8_t p_slot) const override;
	bool alive(uint8_t p_slot) const override;
	const sim::weapons::SoldierWeaponsState &weapons(uint8_t p_slot) const override;
	const sim::CollisionWorld &world() const override { return *world_; }
	sim::Tick tick() const override { return tick_ + 1u; }

	std::span<const DownedSoldier> downed() const override;
	net::MatchPhase phase() const override;
	bool input_restricted() const override;
	uint8_t kit(uint8_t p_slot) const override;

private:
	const SlotTable &slots_;
	const Soldiers &soldiers_;
	const MatchControl &match_;
	const Combat &combat_;
	const sim::Tick &tick_;
	const sim::CollisionWorld *world_ = nullptr;

	mutable std::vector<sim::SoldierState> cache_;
	mutable sim::Tick cache_tick_ = 0;
	mutable bool cached_ = false;
};

}
