#include "bf3/sim/soldier_motor.h"

#include "collision_world_internal.h"
#include "jolt_runtime.h"
#include "vehicles/vehicle_obstacles_internal.h"

#include <Jolt/Math/Trigonometry.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <optional>
#include <span>

namespace bf3::sim {

namespace {

constexpr float kClipTolerance = 1e-4f;

constexpr float kSpeedSnap = 1e-4f;

constexpr float kHugProbeRadius = 0.05f;

constexpr float kFootprintFloorClearance = 0.1f;

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

int pose_rank(Pose p_pose) {
	return p_pose == Pose::Stand ? 2 : (p_pose == Pose::Crouch ? 1 : 0);
}

Pose smaller_pose(Pose p_a, Pose p_b) {
	return pose_rank(p_a) <= pose_rank(p_b) ? p_a : p_b;
}

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

const StateSpeed &speed_of(const MotorParams &p_params, Pose p_pose, bool p_grounded, MoveMode p_mode) {
	const PoseParams &pp = p_params.poses[size_t(p_pose)];
	if (p_grounded) {
		return pp.on_ground;
	}
	return p_mode == MoveMode::Falling ? pp.falling : pp.in_air;
}

const StateSpeed &move_speed(const MotorParams &p_params, const SoldierState &p_state, bool p_grounded) {
	const StateSpeed *speed = &speed_of(p_params, p_state.pose, p_grounded, p_state.mode);
	if (p_state.target_pose != p_state.pose) {
		const StateSpeed &other = speed_of(p_params, p_state.target_pose, p_grounded, p_state.mode);
		if (!p_params.tuning.transition_uses_slower_pose || other.velocity < speed->velocity) {
			speed = &other;
		}
	}
	return *speed;
}

float directional_speed(const StateSpeed &p_speed, float p_forward, float p_right) {
	const float along = p_forward >= 0.0f ? p_speed.forward : p_speed.backward;
	const float side = p_right >= 0.0f ? p_speed.right : p_speed.left;
	const float w_forward = (p_forward * p_forward) / (p_forward * p_forward + p_right * p_right);
	return p_speed.velocity * (w_forward * along + (1.0f - w_forward) * side);
}

}

float walk_speed(const MotorParams &p_params, const SoldierState &p_state) {
	const float yaw = yaw_radians(p_state.yaw);
	const float sy = JPH::Sin(yaw);
	const float cy = JPH::Cos(yaw);
	const float vx = p_state.velocity[0];
	const float vz = p_state.velocity[2];
	const float forward = sy * vx + cy * vz;
	const float right = sy * vz - cy * vx;
	const bool still = forward * forward + right * right <= 0.0f;
	return directional_speed(move_speed(p_params, p_state, true), still ? 1.0f : forward, still ? 0.0f : right) *
			eval(p_params.poses[size_t(p_state.pose)].throttle, 1.0f);
}

struct SoldierMotor::Impl {
	std::shared_ptr<void> runtime;
	const CollisionWorld *world = nullptr;
	MotorParams params;
	std::array<JPH::RefConst<JPH::Shape>, kPoseCount> shapes;
	std::array<float, kPoseCount> shape_height{};
	JPH::Ref<JPH::CharacterVirtual> character;
	const VehicleObstacles *obstacles = nullptr;
	std::unique_ptr<ObstacleContacts> touches = std::make_unique<ObstacleContacts>();

	const CollisionWorld::Impl &w() const { return world->impl(); }

	bool pose_fits(Pose p_pose, const Vec3 &p_feet, uint16_t p_yaw) const;
	bool footprint_free(const Vec3 &p_feet, uint16_t p_yaw) const;
	void begin_pose(SoldierState &r_s, Pose p_to) const;
	void request_move_pose(SoldierState &r_s, uint8_t p_pose) const;
	void begin_vault(SoldierState &r_s, const VaultStart &p_start) const;
	void step_vault(SoldierState &r_s, bool p_blocked, MotorStepInfo *r_info) const;
};

void SoldierMotor::Impl::begin_pose(SoldierState &r_s, Pose p_to) const {
	uint16_t n = params.poses[size_t(r_s.pose)].transition_ticks[size_t(p_to)];
	n = n == 0 ? 1 : n;
	r_s.target_pose = p_to;
	r_s.transition_total = n;
	r_s.transition_ticks = uint16_t(n - 1);
	r_s.events |= soldier_events::kPoseStarted;
	if (r_s.transition_ticks == 0) {
		r_s.pose = r_s.target_pose;
		r_s.events |= soldier_events::kPoseFinished;
	}
}

void SoldierMotor::Impl::request_move_pose(SoldierState &r_s, uint8_t p_pose) const {
	if (p_pose >= kPoseCount || Pose(p_pose) == r_s.target_pose) {
		return;
	}
	const Pose to = Pose(p_pose);
	if (to != Pose::Stand && !pose_fits(to, r_s.position, r_s.yaw)) {
		r_s.events |= soldier_events::kPoseRefused;
		return;
	}
	begin_pose(r_s, to);
}

void SoldierMotor::Impl::begin_vault(SoldierState &r_s, const VaultStart &p_start) const {
	VaultState v;
	v.kind = uint8_t(p_start.kind);
	v.variant = choose_vault_variant(params.vault, p_start.kind, p_start.measure);
	v.yaw = r_s.yaw;
	v.distance = p_start.measure.distance;
	v.height = p_start.measure.height;
	v.length = p_start.measure.length;
	r_s.vault = v;
	r_s.mode = MoveMode::AnimationControlled;
	r_s.events |= soldier_events::kVaulted;
	request_move_pose(r_s, vault_move(params.vault, p_start.kind, v.variant).override_pose);
}

void SoldierMotor::Impl::step_vault(SoldierState &r_s, bool p_blocked, MotorStepInfo *r_info) const {
	const float dt = kTickSeconds;
	const CollisionWorld::Impl &cw = w();
	const uint32_t mask = query_mask::kMovement;
	JPH::CharacterVirtual &ch = *character;
	SoldierState &s = r_s;
	const VaultKind kind = VaultKind(std::min<uint8_t>(s.vault.kind, uint8_t(kVaultKindCount - 1)));
	const SpecialMoveParams &move = vault_move(params.vault, kind, s.vault.variant);
	const std::span<const MoveKey> keys(move.motion.data(), std::min<size_t>(move.motion_count, kMaxMoveKeys));
	const float t0 = float(s.vault.ticks) * dt;
	s.vault.ticks = uint16_t(std::min<uint32_t>(uint32_t(s.vault.ticks) + 1u, 0xFFFFu));
	const float t1 = float(s.vault.ticks) * dt;
	const std::optional<Vec3> from = sample_move_keys(keys, t0);
	const std::optional<Vec3> to = sample_move_keys(keys, t1);
	Vec3 local{};
	if (from && to) {
		local = { (*to)[0] - (*from)[0], (*to)[1] - (*from)[1], (*to)[2] - (*from)[2] };
	}
	const float frame = yaw_radians(move.keep_initial_direction ? s.vault.yaw : s.yaw);
	const float sy = JPH::Sin(frame);
	const float cy = JPH::Cos(frame);
	const VaultMeasure measure{ s.vault.distance, s.vault.height, s.vault.length };
	const VaultScales scales = vault_scales(move, measure);
	const float sh = vault_horizontal_scale(scales, measure, s.vault.moved);
	const float sv = scales.on ? scales.height : 1.0f;
	const JPH::Vec3 motion((sy * local[2] - cy * local[0]) * sh, local[1] * sv, (cy * local[2] + sy * local[0]) * sh);

	const JPH::Vec3 p0 = JPH::Vec3(ch.GetPosition());
	ch.SetLinearVelocity(motion / dt);
	JPH::CharacterVirtual::ExtendedUpdateSettings update;
	update.mStickToFloorStepDown = JPH::Vec3::sZero();
	update.mWalkStairsStepUp = JPH::Vec3::sZero();
	ch.ExtendedUpdate(dt, JPH::Vec3::sZero(), update, cw.broadphase_filter(mask), cw.object_filter(mask),
			cw.body_filter, cw.shape_filter, *cw.temp);
	const JPH::Vec3 p1 = JPH::Vec3(ch.GetPosition());
	const JPH::Vec3 disp = p1 - p0;
	if (s.vault.ticks > 1) {
		s.vault.moved += std::sqrt(disp.GetX() * disp.GetX() + disp.GetZ() * disp.GetZ());
	}
	bool aborted = false;
	for (const JPH::CharacterContact &c : ch.GetActiveContacts()) {
		if (c.mHadCollision &&
				vault_contact_aborts(
						from_jolt(JPH::Vec3(c.mPosition) - p1), from_jolt(c.mContactNormal), from_jolt(motion))) {
			aborted = true;
			break;
		}
	}
	const bool finished =
			aborted || keys.empty() || t1 >= move_duration(move) || motion.Length() / dt < move.exit_velocity;

	const JPH::CharacterBase::EGroundState ground_state = ch.GetGroundState();
	const bool grounded = ground_state == JPH::CharacterBase::EGroundState::OnGround;
	s.position = from_jolt(p1);
	s.velocity = aborted ? Vec3{ 0.0f, 0.0f, 0.0f } : from_jolt(disp / dt);
	if (grounded || ground_state == JPH::CharacterBase::EGroundState::OnSteepGround) {
		const JPH::Vec3 n = ch.GetGroundNormal();
		s.ground_normal = n.LengthSq() > 1e-6f ? from_jolt(n.Normalized()) : Vec3{ 0.0f, 1.0f, 0.0f };
	} else {
		s.ground_normal = { 0.0f, 1.0f, 0.0f };
	}
	if (finished) {
		s.vault = VaultState{};
		s.events |= soldier_events::kVaultEnded;
		s.mode = grounded ? MoveMode::OnGround : MoveMode::InAir;
		request_move_pose(s, move.reset_pose);
	}
	uint16_t flags = soldier_flags::kAlive;
	flags |= grounded ? soldier_flags::kGrounded : 0;
	flags |= p_blocked ? soldier_flags::kStandBlocked : 0;
	flags |= s.protect_ticks > 0 ? soldier_flags::kSpawnProtected : 0;
	s.flags = flags;
	s.air_ticks = grounded ? 0 : uint16_t(std::min<uint32_t>(uint32_t(s.air_ticks) + 1u, 0xFFFFu));
	if (r_info != nullptr) {
		r_info->horizontal_speed = std::sqrt(s.velocity[0] * s.velocity[0] + s.velocity[2] * s.velocity[2]);
		r_info->pose_blocked = p_blocked;
		r_info->contacts = uint32_t(ch.GetActiveContacts().size());
	}
}

bool SoldierMotor::Impl::footprint_free(const Vec3 &p_feet, uint16_t p_yaw) const {
	const PoseParams &prone = params.poses[size_t(Pose::Prone)];
	const float half_x = 0.5f * (prone.box_min_expand[0] + prone.box_max_expand[0]);
	const float half_z = 0.5f * (prone.box_min_expand[2] + prone.box_max_expand[2]);
	const float top = params.tuning.prone_footprint_height;
	if (!params.tuning.prone_footprint_check || half_x <= 0.0f || half_z <= 0.0f || top <= kFootprintFloorClearance) {
		return true;
	}

	const float center_x = 0.5f * (prone.box_max_expand[0] - prone.box_min_expand[0]);
	const float center_z = 0.5f * (prone.box_max_expand[2] - prone.box_min_expand[2]);
	const float yaw = yaw_radians(p_yaw);
	const float s = JPH::Sin(yaw);
	const float c = JPH::Cos(yaw);

	const Vec3 center = { p_feet[0] + center_x * c + center_z * s, p_feet[1] + 0.5f * (top + kFootprintFloorClearance),
		p_feet[2] - center_x * s + center_z * c };
	const Vec3 half = { half_x, 0.5f * (top - kFootprintFloorClearance), half_z };
	return !world->overlap_box(center, half, yaw, query_mask::kMovement, params.tuning.pose_change_max_penetration);
}

bool SoldierMotor::Impl::pose_fits(Pose p_pose, const Vec3 &p_feet, uint16_t p_yaw) const {

	const Vec3 bottom = { p_feet[0], p_feet[1] + params.tuning.character_padding, p_feet[2] };
	if (world->overlap_capsule(bottom, params.radius, shape_height[size_t(p_pose)], query_mask::kMovement, params.tuning.pose_change_max_penetration)) {
		return false;
	}
	return p_pose != Pose::Prone || footprint_free(p_feet, p_yaw);
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

	const Vec3 top = p_world.shapes()->max();
	const JPH::RVec3 far(0.0f, std::max(top[1], 0.0f) + 10000.0f, 0.0f);
	m.character = new JPH::CharacterVirtual(&settings, far, JPH::Quat::sIdentity(), 0, w.system.get());
	set_obstacles(m.obstacles);

	const uint32_t mask = query_mask::kMovement;
	m.character->SetLinearVelocity(JPH::Vec3::sZero());
	m.character->Update(kTickSeconds, JPH::Vec3(0.0f, -p_params.tuning.gravity, 0.0f), w.broadphase_filter(mask), w.object_filter(mask), w.body_filter,
			w.shape_filter, *w.temp);
	return core::Status::success();
}

void SoldierMotor::set_obstacles(const VehicleObstacles *p_obstacles) {
	impl_->obstacles = p_obstacles;
	impl_->touches->obstacles = p_obstacles != nullptr ? &p_obstacles->impl() : nullptr;
	impl_->touches->contacts.clear();
	if (impl_->character != nullptr) {
		impl_->character->SetCharacterVsCharacterCollision(
				p_obstacles != nullptr ? &p_obstacles->impl().collision : nullptr);
		impl_->character->SetListener(p_obstacles != nullptr ? impl_->touches.get() : nullptr);
	}
}

std::span<const ObstacleContact> SoldierMotor::obstacle_contacts() const {
	return impl_->touches->contacts;
}

MotorModifiers motor_modifiers(const MotorParams &p_params) {
	MotorModifiers out;
	out.body_sprint_multiplier = p_params.body_sprint_multiplier;
	return out;
}

void SoldierMotor::step(SoldierState &r_state, const Command &p_command, MotorStepInfo *r_info) const {
	step(r_state, p_command, motor_modifiers(impl_->params), r_info);
}

void SoldierMotor::step(SoldierState &r_state, const Command &p_command, const MotorModifiers &p_modifiers,
		MotorStepInfo *r_info) const {
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
	m.touches->contacts.clear();
	if (r_info != nullptr) {
		*r_info = MotorStepInfo();
	}
	if (m.character == nullptr || (s.flags & soldier_flags::kAlive) == 0 || s.mode == MoveMode::Dead || !finite3(s.position, kMaxCoordinate) ||
			!finite3(s.velocity, kMaxSpeed) || uint8_t(s.pose) >= kPoseCount || uint8_t(s.target_pose) >= kPoseCount) {
		return;
	}
	if (s.mode != MoveMode::AnimationControlled) {
		s.vault = VaultState{};
	}

	if (s.protect_ticks > 0) {
		--s.protect_ticks;
	}

	const bool abort_moved = p.protect_abort_move && (p_command.move_forward != 0 || p_command.move_right != 0);
	if (s.protect_ticks > 0 && ((p_command.buttons & p.protect_abort_buttons) != 0 || abort_moved)) {
		s.protect_ticks = 0;
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

		const bool prone_still = s.pose == Pose::Prone && s.target_pose == Pose::Prone;
		if (!prone_still || m.footprint_free(s.position, p_command.yaw) || !m.footprint_free(s.position, s.yaw)) {
			s.yaw = p_command.yaw;
		}
	}

	bool blocked = (s.flags & soldier_flags::kStandBlocked) != 0;
	if (s.transition_ticks > 0) {
		--s.transition_ticks;
		if (s.transition_ticks == 0) {

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
	} else if (s.mode != MoveMode::AnimationControlled) {
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

		} else if (m.pose_fits(requested, s.position, s.yaw)) {
			m.begin_pose(s, requested);
			blocked = false;
		} else {
			if (!blocked) {
				s.events |= soldier_events::kPoseRefused;
			}
			blocked = true;
		}
	}
	const Pose move_pose = smaller_pose(s.pose, s.target_pose);

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
	if (s.mode != MoveMode::AnimationControlled && p.vault.present && (p_command.buttons & buttons::kJump) != 0 &&
			grounded0 && s.pose == Pose::Stand && s.target_pose == Pose::Stand && s.transition_ticks == 0 &&
			vault_input_allows(forward_in)) {
		const Vec3 facing{ JPH::Sin(yaw), 0.0f, JPH::Cos(yaw) };
		if (const std::optional<VaultStart> start = probe_vault(*m.world, s.position, facing, mask)) {
			m.begin_vault(s, *start);
		}
	}
	if (s.mode == MoveMode::AnimationControlled) {
		m.step_vault(s, blocked, r_info);
		return;
	}

	float target[2] = { 0.0f, 0.0f };
	float target_speed = 0.0f;
	const StateSpeed *speed = &move_speed(p, s, grounded0);
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
		float value = directional_speed(*speed, f, rgt) * throttle;
		if (sprinting) {
			value *= speed->sprint_multiplier * p_modifiers.body_sprint_multiplier;
		}

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

	JPH::CharacterVirtual::ExtendedUpdateSettings update;
	update.mStickToFloorStepDown = JPH::Vec3::sZero();
	update.mWalkStairsStepUp = airborne ? JPH::Vec3::sZero() : JPH::Vec3(0.0f, p.poses[size_t(move_pose)].step_height, 0.0f);
	ch.ExtendedUpdate(dt, JPH::Vec3(0.0f, -g, 0.0f), update, w.broadphase_filter(mask), w.object_filter(mask), w.body_filter, w.shape_filter, *w.temp);

	const bool hug = grounded0 && !jumped && p.ground_hugging && p.allowed_distance_from_ground > 0.0f;
	if (hug && ch.GetGroundState() != JPH::CharacterBase::EGroundState::OnGround && JPH::Vec3(ch.GetPosition()).GetY() - p0.GetY() <= 1e-6f) {
		const JPH::Vec3 at = JPH::Vec3(ch.GetPosition());
		const float reach = p.allowed_distance_from_ground;
		if (m.world->sweep_capsule({ at.GetX(), at.GetY() + 0.01f, at.GetZ() }, kHugProbeRadius, 2.0f * kHugProbeRadius, { 0.0f, -(reach + 0.01f), 0.0f }, mask)) {
			ch.StickToFloor(JPH::Vec3(0.0f, -reach, 0.0f), w.broadphase_filter(mask), w.object_filter(mask), w.body_filter, w.shape_filter, *w.temp);
		}
	}

	const JPH::Vec3 p1 = JPH::Vec3(ch.GetPosition());
	const JPH::Vec3 disp = p1 - p0;
	const JPH::CharacterBase::EGroundState ground_state = ch.GetGroundState();
	const bool grounded1 = ground_state == JPH::CharacterBase::EGroundState::OnGround && !jumped;
	const bool steep = ground_state == JPH::CharacterBase::EGroundState::OnSteepGround;
	const JPH::Vec3 v_after = ch.GetLinearVelocity();
	float vx = v_after.GetX();
	float vz = v_after.GetZ();
	{
		float ex = v_mid.GetX() * dt;
		float ez = v_mid.GetZ() * dt;
		if (!airborne) {

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
			vy = disp.GetY() / dt - 0.5f * g * dt;
		} else {
			vy = vy0 - g * dt;
		}
	}

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

	s.mode = MoveMode::InAir;
	s.flags = soldier_flags::kAlive;
	s.protect_ticks = p_protect ? p.immortal_ticks : 0;
	if (s.protect_ticks > 0) {
		s.flags |= soldier_flags::kSpawnProtected;
	}
	s.health = p.max_health;
	return s;
}

float eye_height(const MotorParams &p_params, const SoldierState &p_state) {
	const size_t from = std::min<size_t>(uint8_t(p_state.pose), kPoseCount - 1);
	const size_t to = std::min<size_t>(uint8_t(p_state.target_pose), kPoseCount - 1);
	const float a = p_params.poses[from].eye_height;
	if (from == to || p_state.transition_ticks == 0 || p_state.transition_total == 0) {
		return from == to ? a : p_params.poses[to].eye_height;
	}
	const float progress = 1.0f - float(std::min(p_state.transition_ticks, p_state.transition_total)) / float(p_state.transition_total);
	return a + (p_params.poses[to].eye_height - a) * progress;
}

float SoldierMotor::eye_height(const SoldierState &p_state) const {
	return sim::eye_height(impl_->params, p_state);
}

bool SoldierMotor::fits_standing(const Vec3 &p_feet) const {
	const Impl &m = *impl_;
	if (m.world == nullptr) {
		return false;
	}

	const float pad = m.params.tuning.character_padding;
	return !m.world->overlap_capsule(p_feet, m.params.radius + pad, m.shape_height[size_t(Pose::Stand)] + 2.0f * pad, query_mask::kMovement, 1e-3f);
}

}
