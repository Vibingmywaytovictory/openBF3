#pragma once
// SoldierState: everything the soldier motor carries from one tick to the next, as one plain struct (plan D6
// "custom fixed-tick motor with POD state"; §3.1 C++ design; topics/05 §5.1 "no hidden state outside state").
// Restoring a soldier is a copy of this struct; the motor's step is a pure function of (state, command, static
// world, params), docs/design/m1.md §6. The server sends the owner this struct at full precision (bf3_net
// protocol.h own-state block); others get a quantised subset.

#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <type_traits>

namespace bf3::sim {

// CharacterStateData types of plan §3.1 / topics/02 §1.3. M1 enters OnGround, Jumping, InAir and Falling; the
// others exist so the wire format and the presentation need no change when ladders, swimming, parachutes and
// vaults arrive (M7); in M1 the motor never enters them.
enum class MoveMode : uint8_t {
	OnGround = 0,
	Jumping = 1, // rising after a jump take-off, until the apex or a landing
	InAir = 2, // airborne, vertical speed above -FreeFallVelocity
	Falling = 3, // airborne, falling faster than FreeFallVelocity (16 m/s)
	Swimming = 4, // stub
	Climbing = 5, // stub (ladders)
	Parachute = 6, // stub
	AnimationControlled = 7, // stub (vault, dive)
	Dead = 8, // health reached 0; the server respawns the slot
};
inline constexpr size_t kMoveModeCount = 9;

namespace soldier_flags {
inline constexpr uint16_t kAlive = 1u << 0;
inline constexpr uint16_t kGrounded = 1u << 1; // supported by walkable ground at the end of the tick
inline constexpr uint16_t kSprinting = 1u << 2;
inline constexpr uint16_t kStandBlocked = 1u << 3; // a requested taller pose has no room (stays until it has)
inline constexpr uint16_t kSteepGround = 1u << 4; // touching ground steeper than MaxAscendAngle
inline constexpr uint16_t kSpawnProtected = 1u << 5; // ImmortalTimeAfterSpawn running (no damage in M1 anyway)
} // namespace soldier_flags

// What happened during the tick that produced the state (cleared at the start of every step).
namespace soldier_events {
inline constexpr uint16_t kJumped = 1u << 0;
inline constexpr uint16_t kLanded = 1u << 1; // impact_speed / damage describe it
inline constexpr uint16_t kDamaged = 1u << 2;
inline constexpr uint16_t kDied = 1u << 3;
inline constexpr uint16_t kPoseStarted = 1u << 4;
inline constexpr uint16_t kPoseFinished = 1u << 5;
inline constexpr uint16_t kPoseRefused = 1u << 6; // a pose change was asked for but had no room
inline constexpr uint16_t kSpawned = 1u << 7;
} // namespace soldier_events

struct SoldierState {
	Tick tick = 0; // the tick this state is the result of (the spawn tick for a fresh spawn)
	uint32_t life = 0; // +1 per spawn of the slot (IW4L reconciliation identity (ClientId, LifeSequence))
	Vec3 position{}; // feet: the base of the movement shape, Frostbite metres
	Vec3 velocity{}; // m/s
	Vec3 ground_normal{ 0.0f, 1.0f, 0.0f }; // of the supporting contact; (0,1,0) when airborne
	uint16_t yaw = 0; // the view of the last command applied (Command quantisation)
	int16_t pitch = 0;
	Pose pose = Pose::Stand; // the pose being left during a transition, else the current pose
	Pose target_pose = Pose::Stand; // == pose when no transition runs
	uint16_t transition_ticks = 0; // ticks left of the running transition (0 = none)
	uint16_t transition_total = 0; // its length in ticks (the TransitionTimes entry, docs §6.5)
	MoveMode mode = MoveMode::OnGround;
	uint8_t reserved0 = 0;
	uint16_t flags = 0; // soldier_flags::
	uint16_t events = 0; // soldier_events:: of the tick that produced this state
	uint16_t jump_penalty_ticks = 0; // ticks left of JumpPenaltyTime after the last take-off
	uint16_t air_ticks = 0; // ticks since the last grounded tick (saturating)
	uint16_t protect_ticks = 0; // ticks left of ImmortalTimeAfterSpawn
	uint16_t reserved1 = 0;
	float health = 100.0f; // exact (owner-only on the wire)
	float impact_speed = 0.0f; // vertical speed at the last landing (m/s, >= 0), for the fall-damage curve
	float damage = 0.0f; // damage taken this tick (fall damage in M1)

	bool operator==(const SoldierState &) const = default;
};
static_assert(std::is_trivially_copyable_v<SoldierState>);

// The command a slot "last had" when its client has sent none for the ticks after p_state (after Spawned, after a
// gap the jitter buffer cannot fill, docs §8.2 / §9.2): the idle command of p_state's tick with its view and target
// pose. The server feeds it to repeated_command for every tick without a real command; the client's catch-up
// replays the same commands, so both step identical inputs.
constexpr Command hold_command(const SoldierState &p_state) {
	return idle_command(p_state.tick, p_state.yaw, p_state.pitch, p_state.target_pose);
}

// FNV-1a 64 over every field's bytes in declaration order (floats as their bit patterns): the determinism tests
// compare runs and the server/client replay with it (soldier_motor.cpp).
uint64_t soldier_state_hash(const SoldierState &p_state);

} // namespace bf3::sim
