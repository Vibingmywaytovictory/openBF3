#pragma once

#include "bf3/core/math.h"
#include "bf3/core/result.h"
#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/vault.h"
#include "bf3/sim/vehicles/vehicle_obstacles.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace bf3::core::gameplay {
struct SoldierDef;
}

namespace bf3::sim {

class CollisionWorld;
class WaterSurface;

enum class GainModel : uint8_t {

	FractionPerReferenceTick = 0,

	Linear = 1,

	Instant = 2,
};

struct MotorTuning {
	// SOURCE: notes R4-B D and its checker (the character's initial gravity (0, -9.81, 0) m/s^2) and R-A0 (BF3's 30 Hz tick).
	// INFERRED: no later writer changes that gravity, and a data gain is per 30 Hz tick. Settle: R4-B D and R-B1 G.
	float gravity = 9.81f;
	float reference_hz = 30.0f;
	// SOURCE: notes R-B1 G (a gain is the fraction closed per tick) and its checker C8 (a negative DecelerationGain moves
	// the velocity by |gain| m/s^2), the three models below.
	// STAND-IN: Linear also runs a positive gain (Swimming's 0.8) and off the ground, where the checker's branch is
	// OnGround with a negative gain (bf3dump soldier: -15). Settle: R-B1 checker C8.
	GainModel acceleration_model = GainModel::FractionPerReferenceTick;
	GainModel deceleration_model = GainModel::Linear;
	GainModel sprint_model = GainModel::FractionPerReferenceTick;
	float air_control_scale = 1.0f;

	// INFERRED: during a pose transition the slower pose's speeds apply. Settle: a note on speeds during a transition.
	bool transition_uses_slower_pose = true;

	// STAND-IN: JumpPenaltyFactor scales a repeated jump's height, where notes R-B2 E let the landing penalty scale speeds.
	// Settle: R-B2 E.
	bool jump_penalty_scales_height = true;

	bool sprint_stands_up = true;

	// STAND-IN: the prone CollisionBoxMin/MaxExpand footprint, 0.35 m high, must be free to go prone and to turn.
	// Settle: a note on the prone collision box.
	bool prone_footprint_check = true;
	float prone_footprint_height = 0.35f;

	// STAND-IN: Jolt CharacterVirtual settings (the seven fields below) in place of BF3's character proxy.
	// Settle: notes R-B1 J's hull and proxy.
	float character_padding = 0.02f;
	float predictive_contact_distance = 0.1f;
	float penetration_recovery_speed = 1.0f;
	float pose_change_max_penetration = 0.01f;
	uint32_t max_collision_iterations = 5;
	uint32_t max_constraint_iterations = 15;
	bool enhanced_internal_edge_removal = true;
	// STAND-IN: an impact below 0.5 m/s is no landing. Settle: a note on the landing threshold.
	float min_landing_speed = 0.5f;

	bool operator==(const MotorTuning &) const = default;
};

inline constexpr size_t kMaxMotorTuningBytes = 64 * 1024;
inline constexpr size_t kMaxMotorTuningLines = 1024;

core::Result<MotorTuning> parse_motor_tuning(std::string_view p_text);
std::string format_motor_tuning(const MotorTuning &p_tuning);

struct CurvePoint {
	float x = 0;
	float y = 0;
};
inline constexpr size_t kMaxCurvePoints = 8;
struct Curve {
	std::array<CurvePoint, kMaxCurvePoints> points{};
	uint8_t count = 0;
};

struct StateSpeed {
	float velocity = 0;
	float forward = 1, backward = 1, left = 1, right = 1;
	float acceleration_gain = 0, deceleration_gain = 0, sprint_gain = 0;
	float sprint_multiplier = 0;

	float acceleration_factor = 0, deceleration_factor = 0, sprint_factor = 0;
};

struct PoseParams {
	float height = 0;
	float eye_height = 0;
	float step_height = 0;
	core::Vec3 box_min_expand{};
	core::Vec3 box_max_expand{};
	float min_look_pitch = 0, max_look_pitch = 0;
	Curve throttle;
	std::array<uint16_t, kPoseCount> transition_ticks{};
	StateSpeed on_ground;
	StateSpeed in_air;
	StateSpeed falling;
	StateSpeed jumping;
	StateSpeed swimming;
};

struct SwimParams {
	bool present = false;
	float depth_limit = 0;
	float body_under_water = 0;
	bool pose_change_during_transition = false;
};

struct MotorParams {
	// STAND-IN: fallbacks for the fields below until make_motor_params reads the soldier's EBX values.
	// Settle: refuse a soldier without them.
	float radius = 0.35f;
	float mass = 100.0f;
	float max_ascend_degrees = 50.0f;
	float allowed_distance_from_ground = 0.5f;
	bool ground_hugging = true;
	float jump_height = 0.6f;
	uint16_t jump_penalty_ticks = 60;
	float jump_penalty_factor = 0.3f;
	float free_fall_velocity = 16.0f;
	float body_sprint_multiplier = 1.0f;
	float max_health = 100.0f;
	uint16_t immortal_ticks = 120;
	Curve fall_damage_vertical;
	// STAND-IN: read but not applied, so a horizontal impact does no damage. Settle: a note on DamageAtHorizVelocity.
	Curve fall_damage_horizontal;
	std::array<PoseParams, kPoseCount> poses{};
	MotorTuning tuning;
	VaultParams vault;
	SwimParams swim;

	uint32_t protect_abort_buttons = 0;
	bool protect_abort_move = false;
};
static_assert(std::is_trivially_copyable_v<MotorParams>);

core::Result<MotorParams> make_motor_params(const core::gameplay::SoldierDef &p_def, const MotorTuning &p_tuning = {});
float eval(const Curve &p_curve, float p_x);

float walk_speed(const MotorParams &p_params, const SoldierState &p_state);
float sprint_speed(const MotorParams &p_params);

float eye_height(const MotorParams &p_params, const SoldierState &p_state);

uint64_t motor_params_hash(const MotorParams &p_params);

struct MotorModifiers {
	float body_sprint_multiplier = 1.0f;

	bool operator==(const MotorModifiers &) const = default;
};
static_assert(std::is_trivially_copyable_v<MotorModifiers>);

MotorModifiers motor_modifiers(const MotorParams &p_params);

struct MotorStepInfo {
	float target_speed = 0;
	float horizontal_speed = 0;
	float ground_distance = 0;
	bool pose_blocked = false;
	uint32_t contacts = 0;
};

class SoldierMotor {
public:
	SoldierMotor();
	~SoldierMotor();
	SoldierMotor(const SoldierMotor &) = delete;
	SoldierMotor &operator=(const SoldierMotor &) = delete;

	core::Status init(const CollisionWorld &p_world, const MotorParams &p_params);
	const MotorParams &params() const;

	void step(SoldierState &r_state, const Command &p_command, MotorStepInfo *r_info = nullptr) const;
	void step(SoldierState &r_state, const Command &p_command, const MotorModifiers &p_modifiers,
			MotorStepInfo *r_info = nullptr) const;

	SoldierState spawn_state(const Vec3 &p_feet, uint16_t p_yaw, uint32_t p_life, Tick p_tick, bool p_protect = true) const;

	float eye_height(const SoldierState &p_state) const;

	bool fits_standing(const Vec3 &p_feet) const;

	void set_obstacles(const VehicleObstacles *p_obstacles);
	std::span<const ObstacleContact> obstacle_contacts() const;
	void set_water(const WaterSurface *p_water);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
