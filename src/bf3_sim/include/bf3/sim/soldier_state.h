#pragma once

#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <type_traits>

namespace bf3::sim {

enum class MoveMode : uint8_t {
	OnGround = 0,
	Jumping = 1,
	InAir = 2,
	Falling = 3,
	Swimming = 4,
	Climbing = 5,
	Parachute = 6,
	AnimationControlled = 7,
	Dead = 8,
};
inline constexpr size_t kMoveModeCount = 9;

namespace soldier_flags {
inline constexpr uint16_t kAlive = 1u << 0;
inline constexpr uint16_t kGrounded = 1u << 1;
inline constexpr uint16_t kSprinting = 1u << 2;
inline constexpr uint16_t kStandBlocked = 1u << 3;
inline constexpr uint16_t kSteepGround = 1u << 4;
inline constexpr uint16_t kSpawnProtected = 1u << 5;
}

namespace soldier_events {
inline constexpr uint16_t kJumped = 1u << 0;
inline constexpr uint16_t kLanded = 1u << 1;
inline constexpr uint16_t kDamaged = 1u << 2;
inline constexpr uint16_t kDied = 1u << 3;
inline constexpr uint16_t kPoseStarted = 1u << 4;
inline constexpr uint16_t kPoseFinished = 1u << 5;
inline constexpr uint16_t kPoseRefused = 1u << 6;
inline constexpr uint16_t kSpawned = 1u << 7;
}

struct SoldierState {
	Tick tick = 0;
	uint32_t life = 0;
	Vec3 position{};
	Vec3 velocity{};
	Vec3 ground_normal{ 0.0f, 1.0f, 0.0f };
	uint16_t yaw = 0;
	int16_t pitch = 0;
	Pose pose = Pose::Stand;
	Pose target_pose = Pose::Stand;

	uint16_t transition_ticks = 0;
	uint16_t transition_total = 0;
	MoveMode mode = MoveMode::OnGround;
	uint8_t reserved0 = 0;
	uint16_t flags = 0;
	uint16_t events = 0;
	uint16_t jump_penalty_ticks = 0;
	uint16_t air_ticks = 0;
	uint16_t protect_ticks = 0;
	uint16_t reserved1 = 0;
	float health = 100.0f;
	float impact_speed = 0.0f;
	float damage = 0.0f;

	bool operator==(const SoldierState &) const = default;
};
static_assert(std::is_trivially_copyable_v<SoldierState>);

constexpr Command hold_command(const SoldierState &p_state) {
	return idle_command(p_state.tick, p_state.yaw, p_state.pitch, p_state.target_pose);
}

uint64_t soldier_state_hash(const SoldierState &p_state);

}
