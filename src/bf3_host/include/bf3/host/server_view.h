#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <cstdint>
#include <span>

namespace bf3::host {

struct DownedSoldier {
	uint8_t slot = 0;
	uint8_t team = 0;
	uint32_t until_tick = 0;
	sim::Vec3 corpse{};
	uint16_t yaw = 0;
};

class ServerView {
public:
	virtual ~ServerView() = default;

	virtual std::span<const sim::SoldierState> states() const = 0;
	virtual uint8_t team(uint8_t p_slot) const = 0;
	virtual bool alive(uint8_t p_slot) const = 0;
	virtual const sim::weapons::SoldierWeaponsState &weapons(uint8_t p_slot) const = 0;
	virtual const sim::CollisionWorld &world() const = 0;
	virtual sim::Tick tick() const = 0;

	virtual std::span<const DownedSoldier> downed() const { return {}; }
	virtual net::MatchPhase phase() const { return net::MatchPhase::InRound; }
	virtual bool input_restricted() const { return false; }
	virtual uint8_t kit(uint8_t p_slot) const {
		(void)p_slot;
		return 0;
	}
};

}
