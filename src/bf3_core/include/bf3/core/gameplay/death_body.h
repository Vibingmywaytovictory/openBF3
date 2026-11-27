#pragma once

#include "bf3/core/math.h"

#include <cstdint>

namespace bf3::core::gameplay {

// INFERRED: codes 0..3 are HitFront, HitRight, HitLeft, HitBack (R5-DT1 A2 maps them by field order).
// Settle: retail footage of a death from a known side.
enum class DeathHitDirection : uint8_t { Front = 0, Right = 1, Left = 2, Back = 3, None = 4 };
enum class DeathBone : uint8_t { Body = 0, Head = 1, RightArm = 2, LeftArm = 3, RightLeg = 4, LeftLeg = 5 };

// SOURCE: notes R5-DT1 A and B with its checker (0.1 m/s dead zone as the double nearest 0.1f, the 1e-6 test, the mod 5
// counter, RandomValue (k + 1) 2^-24 from 24 bits, the 1.0 s Revived pulse) and the six constants below.
inline constexpr double kDeathSpeedDeadZone = double(0.1f);
inline constexpr float kDeathDirectionMinLengthSq = 1.0e-6f;
inline constexpr uint32_t kDeathAnimationVariants = 5;
inline constexpr uint32_t kDeathRandomBits = 24;
inline constexpr uint32_t kDeathRandomMask = (uint32_t(1) << kDeathRandomBits) - 1;
inline constexpr float kRevivedPulseSeconds = 1.0f;

struct DeathRecord {
	int8_t forward_speed = 0;
	int8_t right_speed = 0;
	DeathHitDirection direction = DeathHitDirection::None;
	DeathBone bone = DeathBone::Body;
	bool explosion = false;
	bool sprinting = false;
	uint8_t animation_index = 0;
	uint32_t random = 0;
	bool valid = false;

	float random_value() const {
		return float((random & kDeathRandomMask) + 1) * (1.0f / float(uint32_t(1) << kDeathRandomBits));
	}
	bool head_shot() const { return bone == DeathBone::Head; }
	bool hit(DeathHitDirection p_direction) const { return direction == p_direction; }
	bool operator==(const DeathRecord &) const = default;
};

struct DeathFrame {
	Vec3 right{ 1.0f, 0.0f, 0.0f };
	Vec3 forward{ 0.0f, 0.0f, 1.0f };
};

DeathFrame death_frame_of_yaw(float p_yaw_radians);

DeathHitDirection death_hit_direction(const DeathFrame &p_frame, const Vec3 &p_damage_direction);

int8_t death_speed_sign(float p_value);

DeathBone death_bone_of_type(uint32_t p_bone_type);

struct DeathInput {
	DeathFrame frame;
	Vec3 velocity{};
	bool has_hit = false;
	Vec3 damage_direction{};
	uint32_t bone_type = 0;
	bool explosion = false;
	bool sprinting = false;
	uint32_t random = 0;
};

class DeathCounter {
public:
	uint8_t next() {
		const uint8_t index = uint8_t(count_ % kDeathAnimationVariants);
		++count_;
		return index;
	}
	uint64_t count() const { return count_; }

private:
	// INFERRED: the counter starts at 0 (R5-DT1 A6). Settle: a note on the counter's start value.
	uint64_t count_ = 0;
};

DeathRecord make_death_record(const DeathInput &p_input, DeathCounter &r_counter);

// SOURCE: notes R5-DT1 E and its checker (0.1 s ramp, 1e-6, 4.0 s, 0.99, two pose copies), R5-DT2 C (settle 0.5 s,
// 0.5 m/s, 0.5 rad/s) and R5-DT2 F (hold 1.0 s, root 0.7 s): the constants below.
inline constexpr float kRagdollFinalRampSeconds = 0.1f;
inline constexpr float kRagdollSettleEpsilon = 1.0e-6f;
inline constexpr float kRagdollForceSeconds = 4.0f;
inline constexpr float kRagdollFullyBlendedIn = 0.99f;
inline constexpr uint32_t kRagdollSettleCopies = 2;
inline constexpr float kRagdollFreezeMinActiveSeconds = 0.5f;
inline constexpr float kRagdollFreezeLinearSpeed = 0.5f;
inline constexpr float kRagdollFreezeAngularSpeed = 0.5f;
inline constexpr float kReviveHoldSeconds = 1.0f;
inline constexpr float kReviveRootSeconds = 0.7f;

class RagdollBlend {
public:
	void activate();
	void deactivate();
	void pose_copied();
	bool copies_pose() const { return !blending_out_ && (weight_ <= 0.0f || copies_ < kRagdollSettleCopies); }
	void update(float p_dt, float p_target);
	void blend_out();

	bool active() const { return active_; }
	bool blending_out() const { return blending_out_; }
	float weight() const { return weight_; }
	float active_time() const { return active_time_; }
	bool fully_blended_in() const { return active_ && !blending_out_ && weight_ >= kRagdollFullyBlendedIn; }
	bool ramping() const { return ramp_ > 0.0f; }
	float root_fraction() const { return root_fraction_; }
	bool root_interpolating() const { return root_started_ && root_left_ > 0.0f; }
	bool blend_out_done() const { return blending_out_ && root_started_ && root_left_ <= 0.0f; }

private:
	void set(float p_weight);

	bool active_ = false;
	bool blending_out_ = false;
	float weight_ = 0.0f;
	float previous_ = 0.0f;
	bool settled_ = false;
	bool ramp_ran_ = false;
	float ramp_ = 0.0f;
	float ramp_rate_ = 0.0f;
	float active_time_ = 0.0f;
	uint32_t copies_ = 0;
	float hold_left_ = 0.0f;
	bool root_started_ = false;
	float root_left_ = 0.0f;
	float root_fraction_ = 1.0f;
};

class RagdollSettle {
public:
	void reset() { *this = RagdollSettle(); }
	bool update(float p_dt, float p_max_linear_speed, float p_max_angular_speed);
	bool frozen() const { return frozen_; }
	float active_time() const { return active_time_; }

private:
	float active_time_ = 0.0f;
	bool frozen_ = false;
};

}
