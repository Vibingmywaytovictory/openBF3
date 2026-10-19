#pragma once
// A read-only view of the server's world for bot brains (docs/design/m2a.md §4.2, §10.2; owner sv). S1's
// CommandSource::command(tick, own state) sees only its own soldier; firefight brains also need enemy positions,
// teams, their own magazine and the static world. GameServer::view() returns this, valid during the command-source
// step (§4.1 step 3) of every tick; brains keep a pointer from their construction. The CommandSource interface is
// unchanged. Server thread only.

#include "bf3/net/protocol.h" // M2b W1.0 (ru): net::MatchPhase
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <cstdint>
#include <span>

namespace bf3::host {

// M2b W1.0 (#10): a dead soldier a teammate's defibrillator can still revive (m2b.md §3.4, §3.6): until until_tick
// (the man-down end, or the auto-respawn tick when that is earlier), unless it deployed.
struct DownedSoldier {
	uint8_t slot = 0;
	uint8_t team = 0;
	uint32_t until_tick = 0;
	sim::Vec3 corpse{}; // the feet where it died
	uint16_t yaw = 0;
};

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
	// M2b W1.0 (docs/design/m2b.md §2.6 #10, §5.5, §5.6; owner ru, bb reads): the TDM rules' part, with default bodies
	// (no rules: nobody downed, InRound, unrestricted, kit 0) so test fakes stay cheap. GameServer's View overrides
	// them from its rules ([view], ru).
	virtual std::span<const DownedSoldier> downed() const { return {}; } // revivable corpses, ascending slot
	virtual net::MatchPhase phase() const { return net::MatchPhase::InRound; }
	virtual bool input_restricted() const { return false; } // the pre-round freeze (bots idle; [cmd] drops it anyway)
	virtual uint8_t kit(uint8_t p_slot) const { // the rules' kit draw of the slot's life (host::KitRole)
		(void)p_slot;
		return 0;
	}
};

} // namespace bf3::host
