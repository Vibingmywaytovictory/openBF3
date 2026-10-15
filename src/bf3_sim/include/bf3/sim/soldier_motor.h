#pragma once
// SoldierMotor: BF3's soldier movement on our own Jolt static world (plan §3.1, D6; topics/02 §1.3-§1.4, §4.2;
// docs/design/m1.md §6). Godot-free.
//
//   state' = step(state, command)   at the fixed tick (kTickSeconds), a pure function of (SoldierState, Command,
//                                    the CollisionWorld's static shapes, MotorParams)
//
// Numbers: every gameplay number comes from the soldier's EBX (bf3_core gameplay/soldier_def.h, read through
// make_motor_params): pose heights, eye heights, step height, look limits, throttle curve, transition times,
// per-state per-pose speeds and direction factors, sprint multipliers, jump height, jump penalty, free-fall
// velocity, MaxAscendAngle, AllowedDistanceFromGround / GroundHugging, the fall-damage curves, max health.
// What the data does not give (the soldier's gravity, how AccelerationGain / DecelerationGain / SprintGain act;
// plan §3.1 "Open", S5 harness item H:S3) is MotorTuning: a config with research defaults, read from
// res://config/motor_tuning.cfg in the game and settable in tests, so measured values drop in without code.
//
// Collide-and-slide: Jolt's CharacterVirtual is the solver (docs §6.1), reset from SoldierState at the start of
// every step (position, rotation = yaw, velocity, shape of the pose; then RefreshContacts) and read back after
// ExtendedUpdate (walk stairs = StepHeight, stick to floor = AllowedDistanceFromGround when GroundHugging), so no
// solver state survives between steps and a step restored from a snapshot equals the step the server ran.
// One SoldierMotor serves any number of soldiers of one owner thread, one after the other.

#include "bf3/core/result.h"
#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace bf3::core::gameplay {
struct SoldierDef;
}

namespace bf3::sim {

class CollisionWorld;

// ---- tuning: what the data does not say -------------------------------------------------------------------

enum class GainModel : uint8_t {
	// The gain is the fraction of the remaining speed difference closed per reference tick (reference_hz):
	// v += (v_target - v) * (1 - (1 - gain)^(reference_hz * dt)). Rate independent; at 30 Hz, 0.35 closes 95 %
	// in 0.23 s. [inferred]
	FractionPerReferenceTick = 0,
	// |gain| is a constant acceleration in m/s^2 towards the target. [inferred]
	Linear = 1,
	// Velocity jumps to the target (gains ignored): for tests of the steady-state speeds.
	Instant = 2,
};

struct MotorTuning {
	float gravity = 9.81f; // m/s^2 [inferred; plan §3.1 Open: no soldier gravity in data]
	float reference_hz = 30.0f; // the rate FractionPerReferenceTick gains are read at (BF3 MaxSimFps 30, plan §3.5)
	GainModel acceleration_model = GainModel::FractionPerReferenceTick; // AccelerationGain (0.35 on foot)
	GainModel deceleration_model = GainModel::Linear; // |DecelerationGain| (15 m/s^2 on foot)
	GainModel sprint_model = GainModel::FractionPerReferenceTick; // SprintGain (0.25 standing)
	float air_control_scale = 1.0f; // multiplies the InAir/Falling acceleration gains (0.1 / 0.05)
	// Speed during a pose transition: the slower of the two poses' (true) or the target pose's (false). [inferred]
	bool transition_uses_slower_pose = true;
	// Second jump within JumpPenaltyTime: JumpPenaltyFactor scales the jump height (true) or the take-off
	// speed (false). [inferred]
	bool jump_penalty_scales_height = true;
	// Sprint input in crouch/prone (SprintMultiplier 0 there) asks for Stand. [inferred, topics/02 §1.3]
	bool sprint_stands_up = true;
	// Prone: the footprint box (CollisionBoxMin/MaxExpand, 0.6 x 2.4 m) must be free to enter prone and to turn.
	bool prone_footprint_check = true;
	float prone_footprint_height = 0.35f; // box height above the feet [inferred]
	// Solver (Jolt CharacterVirtual; docs §6.1).
	float character_padding = 0.02f; // CharacterVirtualSettings::mCharacterPadding
	float predictive_contact_distance = 0.1f;
	float penetration_recovery_speed = 1.0f;
	float pose_change_max_penetration = 0.01f; // SetShape's check when growing to a taller pose
	uint32_t max_collision_iterations = 5;
	uint32_t max_constraint_iterations = 15;
	bool enhanced_internal_edge_removal = true;
	float min_landing_speed = 0.5f; // impact speeds below this are no landing event (stairs, ramps)

	bool operator==(const MotorTuning &) const = default;
};

// "key = value" lines, '#' comments, blank lines; keys are the field names above, enums by name
// (fraction_per_reference_tick | linear | instant), booleans true/false. Unknown keys and bad values are errors
// (the message names the line). Keys left out keep the defaults.
core::Result<MotorTuning> parse_motor_tuning(std::string_view p_text);
std::string format_motor_tuning(const MotorTuning &p_tuning); // every key, parse_motor_tuning reads it back

// ---- params: the data, flattened to plain numbers ------------------------------------------------------------

struct CurvePoint {
	float x = 0;
	float y = 0;
};
inline constexpr size_t kMaxCurvePoints = 8;
struct Curve {
	std::array<CurvePoint, kMaxCurvePoints> points{};
	uint8_t count = 0;
};

// One CharacterStatePoseInfo (speeds of one movement state in one pose).
struct StateSpeed {
	float velocity = 0; // m/s at full forward input
	float forward = 1, backward = 1, left = 1, right = 1; // SpeedModifier
	float acceleration_gain = 0, deceleration_gain = 0, sprint_gain = 0;
	float sprint_multiplier = 0;
};

struct PoseParams {
	float height = 0; // CharacterPoseData.Height (prone 0.3: a footprint, docs §6.5)
	float eye_height = 0;
	float step_height = 0;
	std::array<float, 3> box_min_expand{}; // prone footprint
	std::array<float, 3> box_max_expand{};
	float min_look_pitch = 0, max_look_pitch = 0; // degrees
	Curve throttle; // stick magnitude -> speed scale
	std::array<uint16_t, kPoseCount> transition_ticks{}; // to each pose (0 = none listed / same pose)
	StateSpeed on_ground;
	StateSpeed in_air;
	StateSpeed falling;
	StateSpeed jumping; // Stand only in data; crouch/prone copy it
};

struct MotorParams {
	float radius = 0.35f; // PhysicalRadius
	float mass = 100.0f;
	float max_ascend_degrees = 50.0f;
	float allowed_distance_from_ground = 0.5f;
	bool ground_hugging = true;
	float jump_height = 0.6f;
	uint16_t jump_penalty_ticks = 60; // JumpPenaltyTime
	float jump_penalty_factor = 0.3f;
	float free_fall_velocity = 16.0f; // InAir -> Falling below -this
	float body_sprint_multiplier = 1.0f; // SoldierBodyComponentData.SprintMultiplier (x1.1 with the specialization)
	float max_health = 100.0f;
	uint16_t immortal_ticks = 120; // ImmortalTimeAfterSpawn
	Curve fall_damage_vertical; // m/s -> hp
	Curve fall_damage_horizontal; // [M1: evaluated, not applied; docs §6.8]
	std::array<PoseParams, kPoseCount> poses{};
	MotorTuning tuning;
};
static_assert(std::is_trivially_copyable_v<MotorParams>);

// From the SoldierDef the gameplay reader gives (characters/soldiers/mpsoldier). Fails when a number the motor
// needs is missing (a pose, the OnGround stand speed, the jump height, the fall-damage curve).
core::Result<MotorParams> make_motor_params(const core::gameplay::SoldierDef &p_def, const MotorTuning &p_tuning = {});
float eval(const Curve &p_curve, float p_x); // piecewise linear, clamped at both ends (as eval_curve)

// ---- the motor -----------------------------------------------------------------------------------------------

struct MotorStepInfo {
	float target_speed = 0; // horizontal speed the input asked for this tick
	float horizontal_speed = 0; // after the solver
	float ground_distance = 0; // feet to the supporting surface (0 when grounded)
	bool pose_blocked = false;
	uint32_t contacts = 0; // solver contacts at the end of the tick
};

class SoldierMotor {
public:
	SoldierMotor();
	~SoldierMotor();
	SoldierMotor(const SoldierMotor &) = delete;
	SoldierMotor &operator=(const SoldierMotor &) = delete;

	// Binds to p_world (queried on the caller's thread only) with p_params (copied).
	core::Status init(const CollisionWorld &p_world, const MotorParams &p_params);
	const MotorParams &params() const;

	// One tick: r_state (the result of tick p_command.tick - 1) becomes the result of p_command.tick. Requires
	// p_command.tick == r_state.tick + 1; a dead soldier only advances its tick.
	void step(SoldierState &r_state, const Command &p_command, MotorStepInfo *r_info = nullptr) const;

	// A fresh standing soldier at p_feet facing p_yaw, alive, full health, spawn protection running, at p_tick.
	SoldierState spawn_state(const Vec3 &p_feet, uint16_t p_yaw, uint32_t p_life, Tick p_tick) const;
	// Eye height above the feet for the presentation: the pose's, blended linearly over a running transition.
	float eye_height(const SoldierState &p_state) const;
	// Whether a standing soldier fits at p_feet (overlap test of the stand shape, padding included).
	bool fits_standing(const Vec3 &p_feet) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::sim
