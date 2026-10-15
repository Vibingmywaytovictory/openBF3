// SoldierMotor: BF3's soldier movement on Jolt's CharacterVirtual, reset from the POD state every tick
// (docs/design/m1.md §6; the header comment of soldier_motor.h is the contract this file implements).
//
// One step, in order:
//   1. counters (spawn protection, jump penalty), the view;
//   2. pose: a running transition counts down (tick convention of soldier_state.h); a new request starts when the
//      target shape fits on the CollisionWorld (overlap_capsule, overlap_box for the prone footprint);
//   3. reset the character from the state: SetShape (movement pose, no overlap check) -> SetPosition ->
//      SetRotation -> SetLinearVelocity (state.velocity) -> RefreshContacts;
//   4. the motor's velocity: horizontal target from the input, the gain models, the jump take-off (integrated as
//      airborne), gravity while not grounded, the mid-step velocity v - g dt / 2 for the solver;
//   5. ExtendedUpdate(kTickSeconds) with stick-to-floor (grounded, not jumping, GroundHugging) and walk-stairs;
//   6. read back: position, ground state; the end velocity clipped by what the solver realised; landing, impact
//      speed from the exact fall distance, fall damage; mode and flags.
// The motor's own replicated arithmetic uses no libm function other than sqrt (correctly rounded by IEEE 754):
// sin / cos are Jolt's deterministic Sin / Cos, the gain powers are precomputed in make_motor_params.
#include "bf3/sim/soldier_motor.h"

#include "collision_world_internal.h"
#include "jolt_runtime.h"

#include <Jolt/Math/Trigonometry.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>

namespace bf3::sim {

namespace {

// Realised minus expected displacement beyond this is the solver's doing (a wall, a ceiling, a steep plane, a
// landing), docs §6.1 step 5.
constexpr float kClipTolerance = 1e-4f;
// Speeds within this of the target snap onto it (the fraction model approaches it only asymptotically).
constexpr float kSpeedSnap = 1e-4f;
// The prone footprint box starts this far above the feet, so the floor under a prone soldier is not part of it
// (slopes up to about atan(0.1 / 1.3) = 4.4 degrees fit along the body) [inferred; docs §6.5].
constexpr float kFootprintFloorClearance = 0.1f;
// Hostile or corrupt states (a damaged snapshot) are not stepped: the motor only advances the tick.
constexpr float kMaxCoordinate = 1.0e6f;
constexpr float kMaxSpeed = 500.0f;

bool finite3(const Vec3 &p_v, float p_limit) {
	for (const float f : p_v) {
		if (!std::isfinite(f) || std::fabs(f) > p_limit) {
			return false;
		}
	}
	return true;
}

JPH::Vec3 to_jolt(const Vec3 &p_v) {
	return JPH::Vec3(p_v[0], p_v[1], p_v[2]);
}

Vec3 from_jolt(JPH::Vec3Arg p_v) {
	return { p_v.GetX(), p_v.GetY(), p_v.GetZ() };
}

// Pose shapes ordered by size: the movement shape of a transition is the smaller of the two (docs §6.5: shrink at
// the start, grow at the end).
int pose_rank(Pose p_pose) {
	return p_pose == Pose::Stand ? 2 : (p_pose == Pose::Crouch ? 1 : 0);
}

Pose smaller_pose(Pose p_a, Pose p_b) {
	return pose_rank(p_a) <= pose_rank(p_b) ? p_a : p_b;
}

// Moves the horizontal velocity p_v towards p_target under p_model with the precomputed per-tick factor.
void apply_gain(GainModel p_model, float p_factor, const float p_target[2], float r_v[2]) {
	switch (p_model) {
		case GainModel::FractionPerReferenceTick:
			r_v[0] += (p_target[0] - r_v[0]) * p_factor;
			r_v[1] += (p_target[1] - r_v[1]) * p_factor;
			break;
		case GainModel::Linear: {
			const float dx = p_target[0] - r_v[0];
			const float dz = p_target[1] - r_v[1];
			const float len = std::sqrt(dx * dx + dz * dz);
			if (len <= p_factor) {
				r_v[0] = p_target[0];
				r_v[1] = p_target[1];
			} else {
				const float s = p_factor / len;
				r_v[0] += dx * s;
				r_v[1] += dz * s;
			}
			break;
		}
		case GainModel::Instant:
			r_v[0] = p_target[0];
			r_v[1] = p_target[1];
			break;
	}
	const float dx = p_target[0] - r_v[0];
	const float dz = p_target[1] - r_v[1];
	if (dx * dx + dz * dz <= kSpeedSnap * kSpeedSnap) {
		r_v[0] = p_target[0];
		r_v[1] = p_target[1];
	}
}

} // namespace

struct SoldierMotor::Impl {
	std::shared_ptr<void> runtime; // Jolt's factory and allocator outlive the character and shapes
	const CollisionWorld *world = nullptr;
	MotorParams params;
	std::array<JPH::RefConst<JPH::Shape>, kPoseCount> shapes; // feet at the shape's origin
	std::array<float, kPoseCount> shape_height{}; // total height (prone: the sphere's diameter)
	JPH::Ref<JPH::CharacterVirtual> character;

	const CollisionWorld::Impl &w() const { return world->impl(); }

	bool pose_fits(Pose p_pose, const Vec3 &p_feet, uint16_t p_yaw) const;
	bool footprint_free(const Vec3 &p_feet, uint16_t p_yaw) const;
	const StateSpeed &speed_of(Pose p_pose, bool p_grounded, MoveMode p_mode) const;
};

bool SoldierMotor::Impl::footprint_free(const Vec3 &p_feet, uint16_t p_yaw) const {
	const PoseParams &prone = params.poses[size_t(Pose::Prone)];
	const float half_x = 0.5f * (prone.box_min_expand[0] + prone.box_max_expand[0]);
	const float half_z = 0.5f * (prone.box_min_expand[2] + prone.box_max_expand[2]);
	const float top = params.tuning.prone_footprint_height;
	if (!params.tuning.prone_footprint_check || half_x <= 0.0f || half_z <= 0.0f || top <= kFootprintFloorClearance) {
		return true;
	}
	// Local frame: +Z forward (the yaw), the box from -min_expand to +max_expand.
	const float center_x = 0.5f * (prone.box_max_expand[0] - prone.box_min_expand[0]);
	const float center_z = 0.5f * (prone.box_max_expand[2] - prone.box_min_expand[2]);
	const float yaw = yaw_radians(p_yaw);
	const float s = JPH::Sin(yaw);
	const float c = JPH::Cos(yaw);
	// Rotation about +Y by yaw: local +Z -> (sin, 0, cos), local +X -> (cos, 0, -sin).
	const Vec3 center = { p_feet[0] + center_x * c + center_z * s, p_feet[1] + 0.5f * (top + kFootprintFloorClearance),
		p_feet[2] - center_x * s + center_z * c };
	const Vec3 half = { half_x, 0.5f * (top - kFootprintFloorClearance), half_z };
	return !world->overlap_box(center, half, yaw, query_mask::kMovement, params.tuning.pose_change_max_penetration);
}

bool SoldierMotor::Impl::pose_fits(Pose p_pose, const Vec3 &p_feet, uint16_t p_yaw) const {
	// The character's shape sits character_padding above its position (CharacterVirtual adds the padding along up).
	const Vec3 bottom = { p_feet[0], p_feet[1] + params.tuning.character_padding, p_feet[2] };
	if (world->overlap_capsule(bottom, params.radius, shape_height[size_t(p_pose)], query_mask::kMovement, params.tuning.pose_change_max_penetration)) {
		return false;
	}
	return p_pose != Pose::Prone || footprint_free(p_feet, p_yaw);
}

const StateSpeed &SoldierMotor::Impl::speed_of(Pose p_pose, bool p_grounded, MoveMode p_mode) const {
	const PoseParams &pp = params.poses[size_t(p_pose)];
	if (p_grounded) {
		return pp.on_ground;
	}
	return p_mode == MoveMode::Falling ? pp.falling : pp.in_air;
}

SoldierMotor::SoldierMotor() :
		impl_(std::make_unique<Impl>()) {}

SoldierMotor::~SoldierMotor() = default;

const MotorParams &SoldierMotor::params() const {
	return impl_->params;
}

core::Status SoldierMotor::init(const CollisionWorld &p_world, const MotorParams &p_params) {
	Impl &m = *impl_;
	const auto usable = [](float p_v, float p_max) { return std::isfinite(p_v) && p_v > 0.0f && p_v <= p_max; };
	if (!usable(p_params.radius, 5.0f)) {
		return core::make_error("soldier motor: unusable radius");
	}
	for (size_t i = 0; i < kPoseCount; ++i) {
		if (!usable(p_params.poses[i].height, 10.0f)) {
			return core::make_error("soldier motor: unusable height of pose " + std::to_string(i));
		}
	}
	if (p_params.poses[size_t(Pose::Stand)].height < 2.0f * p_params.radius || p_params.poses[size_t(Pose::Crouch)].height < 2.0f * p_params.radius) {
		return core::make_error("soldier motor: stand or crouch height below the capsule diameter");
	}
	if (!usable(p_params.tuning.gravity, 100.0f) || !(p_params.max_ascend_degrees > 0.0f && p_params.max_ascend_degrees < 90.0f) ||
			!usable(p_params.tuning.character_padding, 0.2f) || !usable(p_params.tuning.predictive_contact_distance, 1.0f) ||
			p_params.tuning.max_collision_iterations == 0 || p_params.tuning.max_collision_iterations > 64 ||
			p_params.tuning.max_constraint_iterations == 0 || p_params.tuning.max_constraint_iterations > 256) {
		return core::make_error("soldier motor: tuning out of range (gravity, slope, padding, contact distance or iterations)");
	}
	m.runtime = jolt_runtime_acquire();
	m.world = &p_world;
	m.params = p_params;
	const float r = p_params.radius;
	for (size_t i = 0; i < kPoseCount; ++i) {
		const float h = i == size_t(Pose::Prone) ? 2.0f * r : p_params.poses[i].height;
		m.shape_height[i] = h;
		JPH::RefConst<JPH::Shape> inner;
		if (h - 2.0f * r <= 1e-6f) {
			inner = new JPH::SphereShape(r);
		} else {
			inner = new JPH::CapsuleShape(0.5f * h - r, r);
		}
		m.shapes[i] = new JPH::RotatedTranslatedShape(JPH::Vec3(0.0f, 0.5f * h, 0.0f), JPH::Quat::sIdentity(), inner);
	}
	JPH::CharacterVirtualSettings settings;
	settings.mShape = m.shapes[size_t(Pose::Stand)];
	settings.mUp = JPH::Vec3::sAxisY();
	// Only contacts on the bottom hemisphere can support (a ceiling or a wall touching the cylinder never makes the
	// soldier "on steep ground").
	settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -r);
	settings.mMaxSlopeAngle = JPH::DegreesToRadians(p_params.max_ascend_degrees);
	settings.mMass = p_params.mass;
	settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
	settings.mCharacterPadding = p_params.tuning.character_padding;
	settings.mPredictiveContactDistance = p_params.tuning.predictive_contact_distance;
	settings.mPenetrationRecoverySpeed = p_params.tuning.penetration_recovery_speed;
	settings.mMaxCollisionIterations = p_params.tuning.max_collision_iterations;
	settings.mMaxConstraintIterations = p_params.tuning.max_constraint_iterations;
	settings.mEnhancedInternalEdgeRemoval = p_params.tuning.enhanced_internal_edge_removal;
	const CollisionWorld::Impl &w = m.w();
	// Far above every tile: priming touches no geometry.
	const Vec3 top = p_world.shapes()->max();
	const JPH::RVec3 far(0.0f, std::max(top[1], 0.0f) + 10000.0f, 0.0f);
	m.character = new JPH::CharacterVirtual(&settings, far, JPH::Quat::sIdentity(), 0, w.system.get());
	// RefreshContacts' steep-slope branch reads mLastDeltaTime before the first Update; Jolt's default (1/60) is
	// kTickSeconds only at 60 Hz, so a fresh motor would differ from a used one at another rate (docs §6.1).
	const uint32_t mask = query_mask::kMovement;
	m.character->SetLinearVelocity(JPH::Vec3::sZero());
	m.character->Update(kTickSeconds, JPH::Vec3(0.0f, -p_params.tuning.gravity, 0.0f), w.broadphase_filter(mask), w.object_filter(mask), w.body_filter,
			w.shape_filter, *w.temp);
	return core::Status::success();
}

void SoldierMotor::step(SoldierState &r_state, const Command &p_command, MotorStepInfo *r_info) const {
	const Impl &m = *impl_;
	const MotorParams &p = m.params;
	const MotorTuning &tun = p.tuning;
	const float dt = kTickSeconds;
	const float g = tun.gravity;
	assert(p_command.tick == r_state.tick + 1 && "SoldierMotor::step: command tick must follow the state tick");
	SoldierState &s = r_state;
	s.tick = p_command.tick;
	s.events = 0;
	s.damage = 0.0f;
	if (r_info != nullptr) {
		*r_info = MotorStepInfo();
	}
	if (m.character == nullptr || (s.flags & soldier_flags::kAlive) == 0 || s.mode == MoveMode::Dead || !finite3(s.position, kMaxCoordinate) ||
			!finite3(s.velocity, kMaxSpeed) || uint8_t(s.pose) >= kPoseCount || uint8_t(s.target_pose) >= kPoseCount) {
		return; // dead (or not a state the motor can step): only the tick advances
	}

	// 1. Counters and the view.
	if (s.protect_ticks > 0) {
		--s.protect_ticks;
	}
	if (s.jump_penalty_ticks > 0) {
		--s.jump_penalty_ticks;
	}
	const bool grounded0 = (s.flags & soldier_flags::kGrounded) != 0;
	const float forward_in = axis_value(p_command.move_forward);
	const float right_in = axis_value(p_command.move_right);
	const bool sprint_held = (p_command.buttons & buttons::kSprint) != 0 && forward_in > 0.0f;
	s.pitch = p_command.pitch;
	if (p_command.yaw != s.yaw) {
		// Turning while prone needs the footprint at the new heading (docs §6.5); a footprint that is already blocked
		// at the old heading does not stop the turn, so a soldier crawled into a corner can still turn out of it.
		const bool prone_still = s.pose == Pose::Prone && s.target_pose == Pose::Prone;
		if (!prone_still || m.footprint_free(s.position, p_command.yaw) || !m.footprint_free(s.position, s.yaw)) {
			s.yaw = p_command.yaw;
		}
	}

	// 2. Pose.
	bool blocked = (s.flags & soldier_flags::kStandBlocked) != 0;
	if (s.transition_ticks > 0) {
		--s.transition_ticks;
		if (s.transition_ticks == 0) {
			// A growing change switches the shape now: it must still fit (the soldier may have moved under a ceiling).
			const bool growing = pose_rank(s.target_pose) > pose_rank(s.pose);
			if (growing && !m.pose_fits(s.target_pose, s.position, s.yaw)) {
				s.transition_ticks = 1;
				blocked = true;
			} else {
				s.pose = s.target_pose;
				s.events |= soldier_events::kPoseFinished;
				blocked = false;
			}
		}
	} else {
		Pose requested = p_command.pose;
		if (uint8_t(requested) >= kPoseCount) {
			requested = s.pose;
		}
		if (tun.sprint_stands_up && sprint_held && requested != Pose::Stand) {
			requested = Pose::Stand;
		}
		if (requested == s.pose) {
			s.target_pose = s.pose;
			blocked = false;
		} else if (requested == Pose::Prone && !grounded0) {
			// Prone waits for the ground (no event: nothing was refused yet).
		} else if (m.pose_fits(requested, s.position, s.yaw)) {
			uint16_t n = p.poses[size_t(s.pose)].transition_ticks[size_t(requested)];
			n = n == 0 ? 1 : n;
			s.target_pose = requested;
			s.transition_total = n;
			s.transition_ticks = uint16_t(n - 1);
			s.events |= soldier_events::kPoseStarted;
			blocked = false;
			if (s.transition_ticks == 0) {
				s.pose = s.target_pose;
				s.events |= soldier_events::kPoseFinished;
			}
		} else {
			if (!blocked) {
				s.events |= soldier_events::kPoseRefused;
			}
			blocked = true;
		}
	}
	const Pose move_pose = smaller_pose(s.pose, s.target_pose);

	// 3. Reset the character from the state (docs §6.1 step 1; the order matters, see soldier_motor.h).
	const CollisionWorld::Impl &w = m.w();
	const uint32_t mask = query_mask::kMovement;
	JPH::CharacterVirtual &ch = *m.character;
	const float yaw = yaw_radians(s.yaw);
	const JPH::Vec3 p0 = to_jolt(s.position);
	ch.SetShape(m.shapes[size_t(move_pose)], FLT_MAX, w.broadphase_filter(mask), w.object_filter(mask), w.body_filter, w.shape_filter, *w.temp);
	ch.SetPosition(JPH::RVec3(p0));
	ch.SetRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(), yaw));
	ch.SetLinearVelocity(to_jolt(s.velocity));
	ch.RefreshContacts(w.broadphase_filter(mask), w.object_filter(mask), w.body_filter, w.shape_filter, *w.temp);
	JPH::Vec3 ground_plane = JPH::Vec3::sAxisY();
	if (ch.IsSupported()) {
		const JPH::Vec3 n = ch.GetGroundNormal();
		if (n.LengthSq() > 1e-6f) {
			ground_plane = n.Normalized();
		}
	}

	// 4. The motor's velocity.
	float target[2] = { 0.0f, 0.0f };
	float target_speed = 0.0f;
	const StateSpeed *speed = &m.speed_of(s.pose, grounded0, s.mode);
	if (s.target_pose != s.pose) {
		const StateSpeed &other = m.speed_of(s.target_pose, grounded0, s.mode);
		if (!tun.transition_uses_slower_pose || other.velocity < speed->velocity) {
			speed = &other;
		}
	}
	float f = forward_in;
	float rgt = right_in;
	const float mag2 = f * f + rgt * rgt;
	const bool sprinting = sprint_held && grounded0 && speed->sprint_multiplier > 0.0f;
	if (mag2 > 0.0f) {
		float mag = std::sqrt(mag2);
		if (mag > 1.0f) {
			f /= mag;
			rgt /= mag;
			mag = 1.0f;
		}
		const float throttle = eval(p.poses[size_t(s.pose)].throttle, mag);
		const float along = f >= 0.0f ? speed->forward : speed->backward;
		const float side = rgt >= 0.0f ? speed->right : speed->left;
		const float w_forward = (f * f) / (f * f + rgt * rgt);
		float value = speed->velocity * (w_forward * along + (1.0f - w_forward) * side) * throttle;
		if (sprinting) {
			value *= speed->sprint_multiplier * p.body_sprint_multiplier;
		}
		// Unit direction in the yaw frame: forward (sin, 0, cos), right (-cos, 0, sin) (command.h axes).
		const float sy = JPH::Sin(yaw);
		const float cy = JPH::Cos(yaw);
		const float inv = 1.0f / mag;
		const float lf = f * inv;
		const float lr = rgt * inv;
		target[0] = (sy * lf - cy * lr) * value;
		target[1] = (cy * lf + sy * lr) * value;
		target_speed = value;
	}
	float vh[2] = { s.velocity[0], s.velocity[2] };
	if (mag2 <= 0.0f) {
		apply_gain(tun.deceleration_model, speed->deceleration_factor, target, vh);
	} else if (sprinting) {
		apply_gain(tun.sprint_model, speed->sprint_factor, target, vh);
	} else {
		apply_gain(tun.acceleration_model, speed->acceleration_factor, target, vh);
	}
	// Jump: grounded, not prone, no transition (docs §6.6). The take-off tick is integrated as airborne.
	const bool jumped = (p_command.buttons & buttons::kJump) != 0 && grounded0 && s.pose != Pose::Prone && s.target_pose != Pose::Prone &&
			s.transition_ticks == 0 && s.target_pose == s.pose;
	float vy0 = grounded0 ? 0.0f : s.velocity[1];
	if (jumped) {
		const bool penalised = s.jump_penalty_ticks > 0;
		float height = p.jump_height;
		if (penalised && tun.jump_penalty_scales_height) {
			height *= p.jump_penalty_factor;
		}
		vy0 = std::sqrt(2.0f * g * height);
		if (penalised && !tun.jump_penalty_scales_height) {
			vy0 *= p.jump_penalty_factor;
		}
		s.jump_penalty_ticks = p.jump_penalty_ticks;
		s.events |= soldier_events::kJumped;
	}
	const bool airborne = !grounded0 || jumped;
	const float vy_mid = airborne ? vy0 - 0.5f * g * dt : 0.0f;
	const JPH::Vec3 v_mid(vh[0], vy_mid, vh[1]);
	ch.SetLinearVelocity(v_mid);

	// 5. The solver.
	JPH::CharacterVirtual::ExtendedUpdateSettings update;
	update.mStickToFloorStepDown = JPH::Vec3::sZero(); // the motor hugs the ground itself, below
	update.mWalkStairsStepUp = airborne ? JPH::Vec3::sZero() : JPH::Vec3(0.0f, p.poses[size_t(move_pose)].step_height, 0.0f);
	ch.ExtendedUpdate(dt, JPH::Vec3(0.0f, -g, 0.0f), update, w.broadphase_filter(mask), w.object_filter(mask), w.body_filter, w.shape_filter, *w.temp);
	// Ground hugging (GroundHugging, AllowedDistanceFromGround; docs §6.7) is the motor's rule rather than
	// ExtendedUpdate's stick-to-floor, for two reasons found on synthetic ledges (sim.motor.unit "walking off"):
	//   - ExtendedUpdate sticks only on its own supported -> unsupported transition, judged from the contacts
	//     RefreshContacts found. The solver lifts a soldier a few millimetres onto a ledge's edge (the edge's tilted
	//     contact), so the next tick starts unsupported by that measure though the motor's state is grounded, and the
	//     soldier would leave a 0.4 m ledge airborne;
	//   - a sweep down stops on the edge the rounded shape still rests on, so stick-to-floor walks the shape down any
	//     ledge in steps of less than 0.5 m (a 0.6 m ledge too).
	// So a grounded, non-jumping soldier that did not end on walkable ground and did not move up sticks to the floor
	// when the floor straight below its feet is within AllowedDistanceFromGround.
	const bool hug = grounded0 && !jumped && p.ground_hugging && p.allowed_distance_from_ground > 0.0f;
	if (hug && ch.GetGroundState() != JPH::CharacterBase::EGroundState::OnGround && JPH::Vec3(ch.GetPosition()).GetY() - p0.GetY() <= 1e-6f) {
		const JPH::Vec3 at = JPH::Vec3(ch.GetPosition());
		const float reach = p.allowed_distance_from_ground;
		if (m.world->ray({ at.GetX(), at.GetY() + 0.01f, at.GetZ() }, { 0.0f, -(reach + 0.01f), 0.0f }, mask)) {
			ch.StickToFloor(JPH::Vec3(0.0f, -reach, 0.0f), w.broadphase_filter(mask), w.object_filter(mask), w.body_filter, w.shape_filter, *w.temp);
		}
	}

	// 6. Read back.
	const JPH::Vec3 p1 = JPH::Vec3(ch.GetPosition());
	const JPH::Vec3 disp = p1 - p0;
	const JPH::CharacterBase::EGroundState ground_state = ch.GetGroundState();
	const bool grounded1 = ground_state == JPH::CharacterBase::EGroundState::OnGround && !jumped;
	const bool steep = ground_state == JPH::CharacterBase::EGroundState::OnSteepGround;
	const JPH::Vec3 v_after = ch.GetLinearVelocity(); // the motor's, minus CancelVelocityTowardsSteepSlopes
	float vx = v_after.GetX();
	float vz = v_after.GetZ();
	{
		float ex = v_mid.GetX() * dt;
		float ez = v_mid.GetZ() * dt;
		if (!airborne) {
			// Sliding along a walkable slope shortens the horizontal step by v dt sin^2: not an obstruction.
			const JPH::Vec3 step = v_mid * dt;
			const JPH::Vec3 along = step - step.Dot(ground_plane) * ground_plane;
			ex = along.GetX();
			ez = along.GetZ();
		}
		const float rx = disp.GetX();
		const float rz = disp.GetZ();
		if (std::fabs(rx - ex) > kClipTolerance || std::fabs(rz - ez) > kClipTolerance) {
			float nx = rx / dt;
			float nz = rz / dt;
			const float wanted2 = vh[0] * vh[0] + vh[1] * vh[1];
			const float got2 = nx * nx + nz * nz;
			if (!airborne && got2 > wanted2 && got2 > 0.0f) {
				// On the ground the solver never makes the soldier faster than the motor asked (walk-stairs' minimum
				// step forward, penetration recovery). Airborne it may: a slide down a steep bank is the solver's.
				const float k = std::sqrt(wanted2 / got2);
				nx *= k;
				nz *= k;
			}
			vx = nx;
			vz = nz;
		}
	}
	float vy = 0.0f;
	if (!grounded1 && airborne) {
		if (std::fabs(disp.GetY() - vy_mid * dt) > kClipTolerance) {
			vy = disp.GetY() / dt - 0.5f * g * dt; // end velocity of constant acceleration over the realised step
		} else {
			vy = vy0 - g * dt;
		}
	}

	// Landing: the motor's own grounded flag goes 0 -> 1; impact speed from the exact fall inside the tick (§6.8).
	if (grounded1 && !grounded0) {
		const float fallen = -disp.GetY();
		const float v_prev = s.velocity[1];
		const float vc2 = v_prev * v_prev + 2.0f * g * fallen;
		const float impact = vc2 > 0.0f ? std::sqrt(vc2) : 0.0f;
		if (impact >= tun.min_landing_speed) {
			s.events |= soldier_events::kLanded;
			s.impact_speed = impact;
			const float damage = eval(p.fall_damage_vertical, impact);
			if (damage >= 1.0f && s.protect_ticks == 0) {
				s.damage = damage;
				s.health = std::max(0.0f, s.health - damage);
				s.events |= soldier_events::kDamaged;
			}
		}
	}

	s.position = from_jolt(p1);
	s.velocity = { vx, vy, vz };
	if (grounded1 || steep) {
		const JPH::Vec3 n = ch.GetGroundNormal();
		s.ground_normal = n.LengthSq() > 1e-6f ? from_jolt(n.Normalized()) : Vec3{ 0.0f, 1.0f, 0.0f };
	} else {
		s.ground_normal = { 0.0f, 1.0f, 0.0f };
	}
	uint16_t flags = soldier_flags::kAlive;
	flags |= grounded1 ? soldier_flags::kGrounded : 0;
	flags |= steep ? soldier_flags::kSteepGround : 0;
	flags |= sprinting ? soldier_flags::kSprinting : 0;
	flags |= blocked ? soldier_flags::kStandBlocked : 0;
	flags |= s.protect_ticks > 0 ? soldier_flags::kSpawnProtected : 0;
	if (grounded1) {
		s.mode = MoveMode::OnGround;
	} else if (jumped || (s.mode == MoveMode::Jumping && vy > 0.0f)) {
		s.mode = MoveMode::Jumping;
	} else if (s.mode == MoveMode::Falling || vy < -p.free_fall_velocity) {
		s.mode = MoveMode::Falling;
	} else {
		s.mode = MoveMode::InAir;
	}
	if (s.health <= 0.0f) {
		s.health = 0.0f;
		s.mode = MoveMode::Dead;
		s.events |= soldier_events::kDied;
		flags = uint16_t(flags & ~(soldier_flags::kAlive | soldier_flags::kSprinting));
	}
	s.flags = flags;
	s.air_ticks = grounded1 ? 0 : uint16_t(std::min<uint32_t>(uint32_t(s.air_ticks) + 1u, 0xFFFFu));

	if (r_info != nullptr) {
		r_info->target_speed = target_speed;
		r_info->horizontal_speed = std::sqrt(vx * vx + vz * vz);
		r_info->pose_blocked = blocked;
		r_info->contacts = uint32_t(ch.GetActiveContacts().size());
		if (!grounded1) {
			StaticHit hit;
			r_info->ground_distance = m.world->ground_below(s.position, 1000.0f, mask, &hit) ? s.position[1] - hit.position[1] : 1000.0f;
		}
	}
}

SoldierState SoldierMotor::spawn_state(const Vec3 &p_feet, uint16_t p_yaw, uint32_t p_life, Tick p_tick, bool p_protect) const {
	const MotorParams &p = impl_->params;
	SoldierState s;
	s.tick = p_tick;
	s.life = p_life;
	s.position = p_feet;
	s.yaw = p_yaw;
	s.pose = Pose::Stand;
	s.target_pose = Pose::Stand;
	// Not grounded until the first step finds the floor (the soldier is put at the floor plus the padding).
	s.mode = MoveMode::InAir;
	s.flags = soldier_flags::kAlive;
	s.protect_ticks = p_protect ? p.immortal_ticks : 0;
	if (s.protect_ticks > 0) {
		s.flags |= soldier_flags::kSpawnProtected;
	}
	s.health = p.max_health;
	return s;
}

float SoldierMotor::eye_height(const SoldierState &p_state) const {
	const MotorParams &p = impl_->params;
	const size_t from = std::min<size_t>(uint8_t(p_state.pose), kPoseCount - 1);
	const size_t to = std::min<size_t>(uint8_t(p_state.target_pose), kPoseCount - 1);
	const float a = p.poses[from].eye_height;
	if (from == to || p_state.transition_ticks == 0 || p_state.transition_total == 0) {
		return from == to ? a : p.poses[to].eye_height;
	}
	const float progress = 1.0f - float(std::min(p_state.transition_ticks, p_state.transition_total)) / float(p_state.transition_total);
	return a + (p.poses[to].eye_height - a) * progress;
}

bool SoldierMotor::fits_standing(const Vec3 &p_feet) const {
	const Impl &m = *impl_;
	if (m.world == nullptr) {
		return false;
	}
	// The stand shape grown by the character padding (the room the solver keeps): bottom at the feet.
	const float pad = m.params.tuning.character_padding;
	return !m.world->overlap_capsule(p_feet, m.params.radius + pad, m.shape_height[size_t(Pose::Stand)] + 2.0f * pad, query_mask::kMovement, 1e-3f);
}

} // namespace bf3::sim
