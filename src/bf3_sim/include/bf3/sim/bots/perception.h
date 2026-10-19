#pragma once
// Bot perception (docs/design/m2b.md §5.1, §2.6 #13; declared in W1.0, owner bb). Pure and Godot-free: a bot sees
// only what it could see or hear (positions with a clear ray, heard shots), never hidden state.
//
// Per bot, per candidate enemy, on the bot's perception tick (every update_ticks ticks, staggered by slot): in the FOV
// (fov_h_deg x fov_v_deg) and within max_range_m -> 2 rays (eye -> head, eye -> chest) against the static world, at
// most max_candidates candidates per bot (nearest first), <= 2 x max_candidates rays per bot-update. A visible
// candidate accumulates detection += dt (1 + moving + firing) / detection_time(distance), detection_time linear from
// detect_near_s at near_m to detect_far_s at far_m [inferred]; detected at 1.0, forgotten forget_s after the last
// sight. Hearing: a shot within hearing_radius_m gives a heading and a position estimate with hearing_error_m of
// error, no rays (AILoudness is -1 in the weapon patches [verified], so the radius is a named default,
// measured = false, Q6). Determinism: no wall clock, slot order, the bot's own seed (m2b.md §5.7).

#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::sim::bots {

struct PerceptionParams {
	float fov_h_deg = 160.0f; // research topics/06-bots.md §3.4
	float fov_v_deg = 90.0f;
	float max_range_m = 120.0f;
	uint32_t update_ticks = 6; // each bot every 6 ticks, staggered by slot (1/6 of the bots per tick)
	uint32_t max_candidates = 4; // nearest first; 2 rays each
	float near_m = 10.0f;
	float detect_near_s = 0.2f; // detection time at near_m [inferred]
	float far_m = 100.0f;
	float detect_far_s = 1.5f; // detection time at far_m [inferred]
	float forget_s = 3.0f; // after the last sight
	float hearing_radius_m = 60.0f; // [inferred, measured = false] (Q6)
	float hearing_error_m = 5.0f; // the heard position's error
	float head_height_m = 1.6f; // eye-to-head ray target above the feet (stand; the pose's eye height when known)
	float chest_height_m = 1.3f; // eye-to-chest ray target above the feet
};

// A shot the bot may hear (from the tick's interval shots).
struct HeardShot {
	uint8_t shooter = 0;
	Vec3 origin{};
	Tick tick = 0;
};

// What one bot knows about one enemy.
struct PerceivedEnemy {
	uint8_t slot = 0;
	float detection = 0.0f; // 0..1; detected at 1
	bool visible = false; // a clear ray at the last perception tick
	bool detected = false;
	bool heard = false; // known only from a shot
	Tick last_seen = 0; // tick of the last clear ray (or the heard shot)
	Vec3 last_position{}; // feet where it was last seen (heard: the estimate)
};

// The world as one bot's perception tick reads it. Spans index by slot.
struct PerceptionInput {
	uint8_t self = 0;
	uint8_t team = 0;
	Tick tick = 0;
	Vec3 eye{};
	uint16_t yaw = 0; // Command scale
	int16_t pitch = 0;
	std::span<const SoldierState> states; // every slot's state (unused slots default)
	std::span<const uint8_t> teams; // 0 = unused slot
	std::span<const uint8_t> alive; // 1 = alive
	std::span<const uint8_t> firing; // 1 = fired in the last interval (detection boost)
	std::span<const HeardShot> shots;
	const CollisionWorld *world = nullptr; // null: no rays (units)
	uint64_t seed = 0; // the bot's stream for the hearing error
};

// The perception tick of a slot (the stagger rule): (tick + slot) % update_ticks == 0.
bool perception_tick(uint8_t p_slot, Tick p_tick, uint32_t p_update_ticks);

class Perception {
public:
	explicit Perception(const PerceptionParams &p_params = {});
	void reset(); // a new life: everything forgotten
	// One perception tick (the caller checks perception_tick). Returns the rays cast.
	uint32_t update(const PerceptionInput &p_input);
	std::span<const PerceivedEnemy> enemies() const; // known enemies, ascending slot
	const PerceivedEnemy *find(uint8_t p_slot) const;
	const PerceptionParams &params() const;

private:
	PerceptionParams params_;
	std::vector<PerceivedEnemy> enemies_;
};

} // namespace bf3::sim::bots
