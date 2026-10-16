#pragma once
// A read-only view of the server's world for bot brains (docs/design/m2a.md §4.2, §10.2; owner sv). S1's
// CommandSource::command(tick, own state) sees only its own soldier; firefight brains also need enemy positions,
// teams, their own magazine and the static world. GameServer::view() returns this, valid during the command-source
// step (§4.1 step 3) of every tick; brains keep a pointer from their construction. The CommandSource interface is
// unchanged. Server thread only.

#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <cstdint>
#include <span>

namespace bf3::host {

class ServerView {
public:
	virtual ~ServerView() = default;
	// Every slot's authoritative state after the previous tick (index = slot; unused slots default).
	virtual std::span<const sim::SoldierState> states() const = 0;
	virtual uint8_t team(uint8_t p_slot) const = 0; // 0 = unused slot
	virtual bool alive(uint8_t p_slot) const = 0;
	virtual const sim::weapons::SoldierWeaponsState &weapons(uint8_t p_slot) const = 0;
	virtual const sim::CollisionWorld &world() const = 0; // the server's CollisionWorld (rays for line of sight)
	virtual sim::Tick tick() const = 0; // the tick being built
};

} // namespace bf3::host
