#pragma once

#include "bf3/core/math.h"
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

enum class GainModel : uint8_t {

	FractionPerReferenceTick = 0,

	Linear = 1,

	Instant = 2,
};

struct MotorTuning {
	float gravity = 9.81f;
	float reference_hz = 30.0f;
	GainModel acceleration_model = GainModel::FractionPerReferenceTick;
	GainModel deceleration_model = GainModel::Linear;
	GainModel sprint_model = GainModel::FractionPerReferenceTick;
	float air_control_scale = 1.0f;

	bool transition_uses_slower_pose = true;

	bool jump_penalty_scales_height = true;

	bool sprint_stands_up = true;

	bool prone_footprint_check = true;
	float prone_footprint_height = 0.35f;

	float character_padding = 0.02f;
	float predictive_contact_distance = 0.1f;
	float penetration_recovery_speed = 1.0f;
	float pose_change_max_penetration = 0.01f;
	uint32_t max_collision_iterations = 5;
	uint32_t max_constraint_iterations = 15;
	bool enhanced_internal_edge_removal = true;
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
};

struct MotorParams {
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
	Curve fall_damage_horizontal;
	std::array<PoseParams, kPoseCount> poses{};
	MotorTuning tuning;

	uint32_t protect_abort_buttons = 0;
	bool protect_abort_move = false;
};
static_assert(std::is_trivially_copyable_v<MotorParams>);

core::Result<MotorParams> make_motor_params(const core::gameplay::SoldierDef &p_def, const MotorTuning &p_tuning = {});
float eval(const Curve &p_curve, float p_x);

uint64_t motor_params_hash(const MotorParams &p_params);

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

	SoldierState spawn_state(const Vec3 &p_feet, uint16_t p_yaw, uint32_t p_life, Tick p_tick, bool p_protect = true) const;

	float eye_height(const SoldierState &p_state) const;

	bool fits_standing(const Vec3 &p_feet) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
