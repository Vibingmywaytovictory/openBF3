#pragma once
// The per-soldier weapon state machine at the 60 Hz tick with sub-tick shot times (docs/design/m2a-weapons.md §5.2-
// §5.5; plan §3.2 "Fixed-tick fire sim shared by server, predicting client and bots", D7; topics/01 §2.5, §5.2).
//
// Like the motor (soldier_motor.h), a step is a pure function: step_soldier_weapons(table, state, command,
// context, match seed) -> state', shots, events, with all state in the trivially copyable SoldierWeaponsState.
// The server runs it after each soldier's motor step; the shooter's client runs the same code in its prediction
// and replays it on reconciliation (the state goes into the prediction ring next to SoldierState); bots write the
// same Command. Nothing reads Command::sequence or flags (the M1 rule: repeated and local commands step alike).
// The client keeps the shots and events of a tick's FIRST prediction only: the ShotEvents and timed events a
// catch_up / reconciliation replay produces again are discarded, so the 1p effects and sounds never fire twice
// (docs §7.2). With WeaponTable::infinite_magazine (oracle runs only) the magazine never decreases.
//
// Per tick (command for tick T, interval [tick_start(T), tick_end(T))):
//   1. inputs: weapon_input(command); the fire-mode counter cycles the mode fire_mode_steps(old, new) times
//      ((new - old) mod 4, so a counter that moved by 2 between two received commands cycles twice); a select_slot other than the
//      current one starts a switch (the old weapon's reload is cancelled when rlWeaponSwitchCancelsUnfinishedReload
//      and its threshold has not passed), then the new weapon deploys for DeployTime;
//   2. phase timers that end inside the tick end at their exact time (deploy, bolt, reload, shell, hold);
//   3. shots: while the fire rules allow (§5.3: auto while held, single per press, a burst of N per press, bolt /
//      pump after the cycle, hold-and-release on release), each shot at t = max(next allowed time, tick start),
//      decays spread to t, draws its direction from shot_seed(match seed, player slot, shot index), then kicks
//      spread and recoil; at most kMaxShotsPerTick per tick;
//   4. an empty magazine with AlwaysAutoReload starts the reload when the weapon is next ready; a reload request
//      starts one when the magazine is not full and there are rounds in reserve;
//   5. recoil recovers by one whole tick (spread_recoil.h); the tick's events are ORed into the state.
// Time in the state is absolute SimTime, so a state restored from the ring needs no re-basing.

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
	None = 0, // no weapon in the slot
	Deploying = 1, // DeployTime after a switch or a spawn
	Ready = 2, // may fire at next_shot_time
	Bolt = 3, // the bolt / pump cycle (or waiting for the fire release that starts it)
	Reloading = 4, // magazine reload, or rtSingleBullet's ReloadDelay before the first shell
	ReloadShell = 5, // rtSingleBullet: one shell per ReloadTime
	Holding = 6, // hold-and-release: trigger held (cooking / power)
	Releasing = 7, // hold-and-release: thrown, the projectile spawns after SpawnDelay
};

namespace slot_flags {
inline constexpr uint8_t kTriggerDown = 1u << 0; // the previous command held fire (single fire, bursts, the bolt hold)
inline constexpr uint8_t kFirstShotArmed = 1u << 1; // the next shot takes FirstShotRecoilMultiplier
inline constexpr uint8_t kBoltWaitsRelease = 1u << 2; // HoldBoltActionUntilFireRelease: the cycle starts on release
inline constexpr uint8_t kReloadCommitted = 1u << 3; // ReloadThreshold passed: the rounds are in
inline constexpr uint8_t kZoomWasDown = 1u << 4;
// A press while the weapon was not ready, remembered while fire stays held (single, burst, bolt, hold start; §5.3).
inline constexpr uint8_t kPressPending = 1u << 5;
// Hold-and-release: fire was released before HoldDelay passed; the throw goes when the delay is reached.
inline constexpr uint8_t kReleasePending = 1u << 6;
} // namespace slot_flags

// One weapon of a soldier. Ammo and the spread / recoil state stay with the slot across switches.
struct WeaponSlotState {
	uint16_t def = kNoWeapon; // index into WeaponTable::defs
	WeaponPhase phase = WeaponPhase::None;
	uint8_t mode = 0; // index into WeaponSimDef::modes
	uint8_t mode_counter = 0; // the last fire-mode counter seen (weapon_input.h)
	uint8_t burst_left = 0; // rounds of the running burst still to fire
	uint8_t flags = 0; // slot_flags::
	uint8_t reserved0 = 0;
	int32_t magazine = 0;
	int32_t reserve = 0; // rounds in spare magazines (magazines x capacity at spawn [inferred])
	SimTime phase_start = 0;
	SimTime phase_end = 0; // when the current timed phase ends (0 = untimed)
	SimTime next_shot_time = 0; // the earliest time the next shot (or burst) may go
	float spread = 0; // degrees, the cone half-angle at spread_time
	SimTime spread_time = 0;
	RecoilOffset recoil{};
	SimTime hold_start = 0; // hold-and-release: when the trigger went down
};
static_assert(std::is_trivially_copyable_v<WeaponSlotState>);

namespace weapon_events {
inline constexpr uint32_t kShot = 1u << 0;
inline constexpr uint32_t kDryFire = 1u << 1; // fire with an empty magazine and nothing in reserve
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
inline constexpr uint32_t kSwitchRefused = 1u << 12; // the asked-for slot holds no weapon
inline constexpr uint32_t kHoldStarted = 1u << 13;
inline constexpr uint32_t kThrown = 1u << 14;
inline constexpr uint32_t kFireBlocked = 1u << 15; // fire held while sprinting / deploying / dead
} // namespace weapon_events

struct SoldierWeaponsState {
	Tick tick = 0; // the tick this state is the result of
	uint32_t player_slot = 0; // the RNG's player id (the server slot)
	uint32_t next_shot_index = 0; // every shot of the slot over the match, never reset (weapon_rng.h)
	uint8_t active = 0; // the slot in hand
	uint8_t pending = 0xFF; // the slot being switched to (0xFF = none)
	uint8_t reserved0 = 0;
	uint8_t reserved1 = 0;
	uint32_t events = 0; // weapon_events:: of the tick that produced this state
	SimTime sprint_recover_until = 0; // fire refused until then after a sprint (Mechanics::sprint_blocks_fire)
	std::array<WeaponSlotState, kMaxWeaponSlots> slots{};

	const WeaponSlotState &current() const { return slots[active < kMaxWeaponSlots ? active : 0]; }
};
static_assert(std::is_trivially_copyable_v<SoldierWeaponsState>);

// What the soldier gives the weapon for one tick (plan §3.1 "what the soldier feeds the weapon"): built from the
// motor's state before and after the tick by make_weapon_context (the integration seam, §7.1).
struct WeaponContext {
	Vec3 eye_start{}; // world eye at tick_start (the previous state's feet + eye height)
	Vec3 eye_end{}; // at tick_end; a sub-tick shot's origin is the lerp at its fraction [inferred]
	uint16_t yaw = 0; // the command's view (Command quantisation), same for every shot of the tick
	int16_t pitch = 0;
	SwayContext sway;
	bool alive = true;
	bool sprinting = false; // soldier_flags::kSprinting after the step
	float suppression = 0; // the soldier's suppression level (0 until M7's model)
};

// One shot (one ShotConfigData firing; pellets are expanded by ProjectileWorld from the same seed). This is what
// the server sends remote clients (plan §3.2 "ShotEvent") and what VFX and audio start from.
struct ShotEvent {
	SimTime time = 0; // exact sub-tick time of the shot
	SimTime spawn_time = 0; // time + SpawnDelay: when the projectile leaves
	uint64_t seed = 0; // shot_seed(match seed, player slot, shot index)
	uint32_t player_slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	uint8_t weapon_slot = 0;
	uint8_t pellets = 1;
	Vec3 origin{}; // world: the eye at `time` + the launch offset turned by the aim
	Vec3 aim{}; // unit: view + recoil, before spread (the tracer converges on it)
	Vec3 direction{}; // unit: after spread (pellet 0 adds its cone draw to this)
	float spread = 0; // the cone half-angle the direction was drawn in (degrees)
	// The pellet cone half-angle in effect (WeaponSimDef::pellet_cone for the shooter's stance and zoom; degrees;
	// 0 for single-projectile shots). Carried so a remote client and the server expand the pellets alike from the
	// event without knowing the shooter's stance (ProjectileWorld::spawn).
	float pellet_cone = 0;
	RecoilOffset recoil{}; // the offset this shot was aimed with (before its own kick)
	bool tracer = false; // trace_frequency > 0 && shot_index % trace_frequency == 0 (<= 0 in data: no tracers)
	float power = 1; // hold-and-release power modifier (grenades)
};
static_assert(std::is_trivially_copyable_v<ShotEvent>);

// One phase event at its exact sub-tick time (reload start / commit / finish, shell, bolt, deploy, hold, throw):
// what W6 measures the reload and bolt durations from, and what the viewmodel and audio time their cues by.
struct TimedWeaponEvent {
	SimTime time = 0;
	uint32_t event = 0; // exactly one weapon_events:: bit
	uint8_t weapon_slot = 0;
};
// Phase events per tick: at most one deploy end, one bolt end, one reload commit and end, one shell, a switch
// (cancel + deploy start) and the shot-related starts; 16 leaves room and is checked by the W8 invariants.
inline constexpr size_t kMaxTimedEventsPerTick = 16;

struct WeaponTickOutput {
	uint8_t shot_count = 0;
	std::array<ShotEvent, kMaxShotsPerTick> shots{};
	uint32_t events = 0; // the same as the state's
	uint32_t refused_shots = 0; // shots the per-tick cap cut (always 0 for a valid def: §9)
	uint8_t timed_count = 0;
	std::array<TimedWeaponEvent, kMaxTimedEventsPerTick> timed{}; // in time order (ties: the order they happened in)
	uint8_t timed_dropped = 0; // events beyond kMaxTimedEventsPerTick (always 0: the W8 invariants check it)
};

// A fresh life: every slot of p_loadout (def ids, kNoWeapon for empty) full, the active slot deploying from
// p_spawn_tick's end. p_state keeps player_slot and next_shot_index across lives. Fails on a def id out of range
// or a loadout longer than kMaxWeaponSlots.
core::Status spawn_soldier_weapons(SoldierWeaponsState &r_state, const WeaponTable &p_table, std::span<const uint16_t> p_loadout,
		uint8_t p_active, Tick p_spawn_tick);

// One tick. p_command.tick must be r_state.tick + 1 (else nothing changes and the precondition counter of the
// caller should count it, as GameServerStats does for the motor).
void step_soldier_weapons(const WeaponTable &p_table, SoldierWeaponsState &r_state, const Command &p_command, const WeaponContext &p_context,
		uint64_t p_match_seed, WeaponTickOutput &r_out);

// FNV-1a 64 over every field, field by field in declaration order (never a memcpy of the struct: SoldierWeaponsState
// and WeaponSlotState have implicit padding before their SimTime fields). The determinism and replay tests compare
// with it, like soldier_state_hash.
uint64_t soldier_weapons_hash(const SoldierWeaponsState &p_state);

// The integration seam (docs §7.1): the context of one tick from the motor's state before and after it.
// Stance = after.pose (the target pose during a transition [inferred]); move = Sprinting / Jumping (Jumping and
// InAir and Falling) / Moving (horizontal speed > 0.1 m/s [inferred]) / Base; zoom = the command's kZoom held.
WeaponContext make_weapon_context(const MotorParams &p_motor, const SoldierState &p_before, const SoldierState &p_after, const Command &p_command,
		float p_suppression = 0.0f);

} // namespace bf3::sim::weapons
