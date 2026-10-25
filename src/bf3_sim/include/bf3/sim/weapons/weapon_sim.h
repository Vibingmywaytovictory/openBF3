#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>

namespace bf3::sim {
struct MotorParams;
}

namespace bf3::sim::weapons {

enum class WeaponPhase : uint8_t {
	None = 0,
	Deploying = 1,
	Ready = 2,
	Bolt = 3,
	Reloading = 4,
	ReloadShell = 5,
	Holding = 6,
	Releasing = 7,
};

namespace slot_flags {
inline constexpr uint8_t kTriggerDown = 1u << 0;
inline constexpr uint8_t kFirstShotArmed = 1u << 1;
inline constexpr uint8_t kBoltWaitsRelease = 1u << 2;
inline constexpr uint8_t kReloadCommitted = 1u << 3;
inline constexpr uint8_t kZoomWasDown = 1u << 4;

inline constexpr uint8_t kPressPending = 1u << 5;

inline constexpr uint8_t kReleasePending = 1u << 6;
}

struct WeaponSlotState {
	uint16_t def = kNoWeapon;
	WeaponPhase phase = WeaponPhase::None;
	uint8_t mode = 0;
	uint8_t mode_counter = 0;
	uint8_t burst_left = 0;
	uint8_t flags = 0;
	uint8_t reserved0 = 0;
	int32_t magazine = 0;
	int32_t reserve = 0;
	SimTime phase_start = 0;
	SimTime phase_end = 0;
	SimTime next_shot_time = 0;
	float spread = 0;
	SimTime spread_time = 0;
	RecoilOffset recoil{};
	SimTime hold_start = 0;
};
static_assert(std::is_trivially_copyable_v<WeaponSlotState>);

namespace weapon_events {
inline constexpr uint32_t kShot = 1u << 0;
inline constexpr uint32_t kDryFire = 1u << 1;
inline constexpr uint32_t kDeployStarted = 1u << 2;
inline constexpr uint32_t kDeployFinished = 1u << 3;
inline constexpr uint32_t kReloadStarted = 1u << 4;
inline constexpr uint32_t kReloadCommitted = 1u << 5;
inline constexpr uint32_t kReloadFinished = 1u << 6;
inline constexpr uint32_t kReloadCancelled = 1u << 7;
inline constexpr uint32_t kShellLoaded = 1u << 8;
inline constexpr uint32_t kBoltStarted = 1u << 9;
inline constexpr uint32_t kBoltFinished = 1u << 10;
inline constexpr uint32_t kFireModeChanged = 1u << 11;
inline constexpr uint32_t kSwitchRefused = 1u << 12;
inline constexpr uint32_t kHoldStarted = 1u << 13;
inline constexpr uint32_t kThrown = 1u << 14;
inline constexpr uint32_t kFireBlocked = 1u << 15;
}

struct SoldierWeaponsState {
	Tick tick = 0;
	uint32_t player_slot = 0;
	uint32_t next_shot_index = 0;
	uint8_t active = 0;
	uint8_t pending = 0xFF;
	uint8_t reserved0 = 0;
	uint8_t reserved1 = 0;
	uint32_t events = 0;
	SimTime sprint_recover_until = 0;
	std::array<WeaponSlotState, kMaxWeaponSlots> slots{};

	const WeaponSlotState &current() const { return slots[active < kMaxWeaponSlots ? active : 0]; }
};
static_assert(std::is_trivially_copyable_v<SoldierWeaponsState>);

struct WeaponContext {
	Vec3 eye_start{};
	Vec3 eye_end{};
	uint16_t yaw = 0;
	int16_t pitch = 0;
	SwayContext sway;
	bool alive = true;
	bool sprinting = false;
	float suppression = 0;
};

struct ShotEvent {
	SimTime time = 0;
	SimTime spawn_time = 0;
	uint64_t seed = 0;
	uint32_t player_slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	uint8_t weapon_slot = 0;
	uint8_t pellets = 1;
	Vec3 origin{};
	Vec3 aim{};
	Vec3 direction{};
	float spread = 0;

	float pellet_cone = 0;
	RecoilOffset recoil{};
	bool tracer = false;
	float power = 1;
};
static_assert(std::is_trivially_copyable_v<ShotEvent>);

struct TimedWeaponEvent {
	SimTime time = 0;
	uint32_t event = 0;
	uint8_t weapon_slot = 0;
};

inline constexpr size_t kMaxTimedEventsPerTick = 16;

struct WeaponTickOutput {
	uint8_t shot_count = 0;
	std::array<ShotEvent, kMaxShotsPerTick> shots{};
	uint32_t events = 0;
	uint32_t refused_shots = 0;
	uint8_t timed_count = 0;
	std::array<TimedWeaponEvent, kMaxTimedEventsPerTick> timed{};
	uint8_t timed_dropped = 0;
};

core::Status spawn_soldier_weapons(SoldierWeaponsState &r_state, const WeaponTable &p_table, std::span<const uint16_t> p_loadout,
		uint8_t p_active, Tick p_spawn_tick);

void step_soldier_weapons(const WeaponTable &p_table, SoldierWeaponsState &r_state, const Command &p_command, const WeaponContext &p_context,
		uint64_t p_match_seed, WeaponTickOutput &r_out);

uint64_t soldier_weapons_hash(const SoldierWeaponsState &p_state);

SwayContext weapon_sway_context(const MotorParams &p_motor, const SoldierState &p_after, const Command &p_command);

WeaponContext make_weapon_context(const MotorParams &p_motor, const SoldierState &p_before, const SoldierState &p_after, const Command &p_command,
		float p_suppression = 0.0f);

}
