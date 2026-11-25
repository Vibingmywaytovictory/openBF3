#include "bf3/core/gameplay/death_body.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::gameplay {

DeathFrame death_frame_of_yaw(float p_yaw_radians) {
	const float s = std::sin(p_yaw_radians);
	const float c = std::cos(p_yaw_radians);
	DeathFrame f;
	f.right = Vec3{ c, 0.0f, -s };
	f.forward = Vec3{ s, 0.0f, c };
	return f;
}

DeathHitDirection death_hit_direction(const DeathFrame &p_frame, const Vec3 &p_damage_direction) {
	Vec3 h{ p_damage_direction.x, 0.0f, p_damage_direction.z };
	const float length_sq = h.x * h.x + h.z * h.z;
	if (!(length_sq > kDeathDirectionMinLengthSq)) {
		h = Vec3{ 1.0f, 0.0f, 0.0f };
	} else {
		const float inv = 1.0f / std::sqrt(length_sq);
		h = Vec3{ h.x * inv, 0.0f, h.z * inv };
	}
	const float rr = vec::dot(p_frame.right, p_frame.right);
	const float scale = rr > 0.0f ? 1.0f / rr : 1.0f;
	const float a = vec::dot(p_frame.right, h) * scale;
	const float b = vec::dot(p_frame.forward, h) * scale;
	if (std::fabs(b) < std::fabs(a)) {
		return a < 0.0f ? DeathHitDirection::Left : DeathHitDirection::Right;
	}
	return b < 0.0f ? DeathHitDirection::Front : DeathHitDirection::Back;
}

int8_t death_speed_sign(float p_value) {
	const double v = double(p_value);
	if (!(std::fabs(v) >= kDeathSpeedDeadZone)) {
		return 0;
	}
	return v > 0.0 ? int8_t(1) : int8_t(-1);
}

DeathBone death_bone_of_type(uint32_t p_bone_type) {
	return p_bone_type <= uint32_t(DeathBone::LeftLeg) ? DeathBone(p_bone_type) : DeathBone::Body;
}

DeathRecord make_death_record(const DeathInput &p_input, DeathCounter &r_counter) {
	DeathRecord r;
	r.forward_speed = death_speed_sign(vec::dot(p_input.frame.forward, p_input.velocity));
	r.right_speed = death_speed_sign(-vec::dot(p_input.frame.right, p_input.velocity));
	r.direction =
			p_input.has_hit ? death_hit_direction(p_input.frame, p_input.damage_direction) : DeathHitDirection::None;
	r.bone = death_bone_of_type(p_input.bone_type);
	r.explosion = p_input.explosion;
	r.sprinting = p_input.sprinting;
	r.animation_index = r_counter.next();
	r.random = p_input.random & kDeathRandomMask;
	r.valid = true;
	return r;
}

void RagdollBlend::activate() {
	active_ = true;
	blending_out_ = false;
	active_time_ = 0.0f;
	ramp_ = 0.0f;
	ramp_ran_ = false;
	settled_ = false;
	previous_ = weight_;
}

void RagdollBlend::deactivate() {
	*this = RagdollBlend();
}

void RagdollBlend::pose_copied() {
	if (copies_pose()) {
		++copies_;
	}
}

void RagdollBlend::set(float p_weight) {
	previous_ = weight_;
	weight_ = p_weight;
	settled_ = std::fabs(weight_ - previous_) <= kRagdollSettleEpsilon && weight_ > 0.0f &&
			copies_ >= kRagdollSettleCopies;
	if (settled_ && !ramp_ran_ && !blending_out_) {
		ramp_ran_ = true;
		ramp_ = kRagdollFinalRampSeconds;
		ramp_rate_ = (1.0f - weight_) / kRagdollFinalRampSeconds;
	}
}

void RagdollBlend::update(float p_dt, float p_target) {
	if (!active_ || !std::isfinite(p_dt) || p_dt < 0.0f) {
		return;
	}
	const float target = std::isfinite(p_target) ? std::clamp(p_target, 0.0f, 1.0f) : 0.0f;
	active_time_ += p_dt;
	if (blending_out_) {
		if (hold_left_ > 0.0f) {
			hold_left_ = std::max(hold_left_ - p_dt, 0.0f);
			if (hold_left_ == 0.0f) {
				root_started_ = true;
				root_left_ = kReviveRootSeconds;
			}
		}
		if (root_started_ && root_left_ > 0.0f) {
			root_left_ = std::max(root_left_ - p_dt, 0.0f);
			root_fraction_ = root_left_ / kReviveRootSeconds;
		}
		if (!settled_) {
			set(std::min(weight_, target));
		}
		return;
	}
	if (ramp_ > 0.0f) {
		ramp_ = std::max(ramp_ - p_dt, 0.0f);
		weight_ = 1.0f - ramp_rate_ * ramp_;
	} else if (!settled_) {
		set(std::max(weight_, target));
	}
	if (active_time_ > kRagdollForceSeconds) {
		weight_ = std::max(weight_, 1.0f);
	}
}

void RagdollBlend::blend_out() {
	if (!active_) {
		return;
	}
	blending_out_ = true;
	settled_ = false;
	ramp_ = 0.0f;
	hold_left_ = kReviveHoldSeconds;
	root_started_ = false;
	root_left_ = 0.0f;
	root_fraction_ = 1.0f;
}

bool RagdollSettle::update(float p_dt, float p_max_linear_speed, float p_max_angular_speed) {
	if (frozen_) {
		return true;
	}
	if (active_time_ > kRagdollFreezeMinActiveSeconds && p_max_linear_speed <= kRagdollFreezeLinearSpeed &&
			p_max_angular_speed <= kRagdollFreezeAngularSpeed) {
		frozen_ = true;
	}
	active_time_ += p_dt;
	return frozen_;
}

}
