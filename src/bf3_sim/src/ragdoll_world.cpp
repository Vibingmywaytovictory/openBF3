#include "bf3/sim/ragdoll_world.h"

#include "jolt_convert.h"
#include "jolt_runtime.h"
#include "static_world_internal.h"

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace bf3::sim {

namespace {

using core::res::RagdollBodyDef;
using core::res::RagdollDef;
using core::res::RagdollJointDef;
using core::res::RagdollJointKind;
using core::res::RagdollShapeKind;

constexpr uint32_t kNoParentBody = UINT32_MAX;
constexpr JPH::BroadPhaseLayer kStaticBroadPhase(0);
constexpr JPH::BroadPhaseLayer kRagdollBroadPhase(1);
constexpr uint32_t kObjectLayers = kRagdollObjectLayer + 1u;

class RagdollBroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
	JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer p_layer) const override {
		return p_layer == kRagdollObjectLayer ? kRagdollBroadPhase : kStaticBroadPhase;
	}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer p_layer) const override {
		return p_layer == kStaticBroadPhase ? "STATIC" : "RAGDOLL";
	}
#endif
};

class RagdollObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer p_layer, JPH::BroadPhaseLayer) const override {
		return p_layer == kRagdollObjectLayer;
	}
};

class RagdollObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer p_a, JPH::ObjectLayer p_b) const override {
		if (p_a != kRagdollObjectLayer && p_b != kRagdollObjectLayer) {
			return false;
		}
		const JPH::ObjectLayer other = p_a == kRagdollObjectLayer ? p_b : p_a;
		return other == kRagdollObjectLayer || other == JPH::ObjectLayer(StaticLayer::Collision);
	}
};

JPH::Quat quat_of(const core::Transform &p_t) {
	const JPH::Mat44 m(JPH::Vec4(p_t.right.x, p_t.right.y, p_t.right.z, 0.0f),
			JPH::Vec4(p_t.up.x, p_t.up.y, p_t.up.z, 0.0f), JPH::Vec4(p_t.forward.x, p_t.forward.y, p_t.forward.z, 0.0f),
			JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
	return m.GetQuaternion().Normalized();
}

JPH::Vec3 point_of(const core::Transform &p_body, const JPH::Vec3 &p_local) {
	return to_jolt(p_body.transform_point(from_jolt(p_local)));
}

JPH::Vec3 axis_of(const core::Transform &p_body, const core::Vec3 &p_local) {
	return to_jolt(p_body.transform_vector(p_local)).Normalized();
}

core::Result<JPH::RefConst<JPH::Shape>> body_shape(const RagdollBodyDef &p_body) {
	JPH::Ref<JPH::ShapeSettings> inner;
	JPH::Vec3 centre = to_jolt(p_body.a);
	JPH::Quat turn = JPH::Quat::sIdentity();
	const JPH::Vec3 axis = to_jolt(p_body.b) - to_jolt(p_body.a);
	const float half = 0.5f * axis.Length();
	if (p_body.shape == RagdollShapeKind::Capsule && half > 1.0e-4f) {
		inner = new JPH::CapsuleShapeSettings(half, p_body.radius);
		centre = 0.5f * (to_jolt(p_body.a) + to_jolt(p_body.b));
		turn = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), axis / (2.0f * half));
	} else {
		inner = new JPH::SphereShapeSettings(p_body.radius);
	}
	JPH::RotatedTranslatedShapeSettings placed(centre, turn, inner);
	JPH::ShapeSettings::ShapeResult r = placed.Create();
	if (r.HasError()) {
		return core::make_error("ragdoll world: shape of " + p_body.name + ": " + std::string(r.GetError().c_str()));
	}
	return JPH::RefConst<JPH::Shape>(r.Get());
}

// INFERRED: a ragdoll joint as a six-degree-of-freedom constraint (twist about X, cone about Y, the planes range
// clipped to the cone about Z, pyramid swing), a limited hinge as a hinge. Settle: a note on Havok's joint limits.
JPH::Ref<JPH::TwoBodyConstraintSettings> joint_settings(const RagdollDef &p_def, const RagdollJointDef &p_joint) {
	const core::Transform &child = p_def.bodies[p_joint.child].rest;
	const core::Transform &parent = p_def.bodies[p_joint.parent].rest;
	const JPH::Vec3 pivot_parent = point_of(parent, to_jolt(p_joint.parent_frame.trans));
	const JPH::Vec3 pivot_child = point_of(child, to_jolt(p_joint.child_frame.trans));
	if (p_joint.kind == RagdollJointKind::LimitedHinge) {
		JPH::Ref<JPH::HingeConstraintSettings> h = new JPH::HingeConstraintSettings();
		h->mSpace = JPH::EConstraintSpace::WorldSpace;
		h->mPoint1 = JPH::RVec3(pivot_parent);
		h->mHingeAxis1 = axis_of(parent, p_joint.parent_frame.right);
		h->mNormalAxis1 = axis_of(parent, p_joint.parent_frame.up);
		h->mPoint2 = JPH::RVec3(pivot_child);
		h->mHingeAxis2 = axis_of(child, p_joint.child_frame.right);
		h->mNormalAxis2 = axis_of(child, p_joint.child_frame.up);
		h->mLimitsMin = p_joint.hinge_min;
		h->mLimitsMax = p_joint.hinge_max;
		h->mMaxFrictionTorque = p_joint.max_friction_torque;
		return JPH::Ref<JPH::TwoBodyConstraintSettings>(h.GetPtr());
	}
	JPH::Ref<JPH::SixDOFConstraintSettings> s = new JPH::SixDOFConstraintSettings();
	s->mSpace = JPH::EConstraintSpace::WorldSpace;
	s->mSwingType = JPH::ESwingType::Pyramid;
	s->mPosition1 = JPH::RVec3(pivot_parent);
	s->mAxisX1 = axis_of(parent, p_joint.parent_frame.right);
	s->mAxisY1 = axis_of(parent, p_joint.parent_frame.up);
	s->mPosition2 = JPH::RVec3(pivot_child);
	s->mAxisX2 = axis_of(child, p_joint.child_frame.right);
	s->mAxisY2 = axis_of(child, p_joint.child_frame.up);
	using Axis = JPH::SixDOFConstraintSettings::EAxis;
	s->MakeFixedAxis(Axis::TranslationX);
	s->MakeFixedAxis(Axis::TranslationY);
	s->MakeFixedAxis(Axis::TranslationZ);
	s->SetLimitedAxis(Axis::RotationX, p_joint.twist_min, p_joint.twist_max);
	s->SetLimitedAxis(Axis::RotationY, -p_joint.cone, p_joint.cone);
	s->SetLimitedAxis(
			Axis::RotationZ, std::max(p_joint.plane_min, -p_joint.cone), std::min(p_joint.plane_max, p_joint.cone));
	for (const Axis a : { Axis::RotationX, Axis::RotationY, Axis::RotationZ }) {
		s->mMaxFriction[a] = p_joint.max_friction_torque;
	}
	return JPH::Ref<JPH::TwoBodyConstraintSettings>(s.GetPtr());
}

}

struct RagdollWorld::Impl {
	std::shared_ptr<void> runtime;
	std::shared_ptr<const StaticWorldShapes> shapes;
	std::shared_ptr<const RagdollDef> def;
	RagdollWorldOptions options;
	std::unique_ptr<RagdollBroadPhaseLayers> broadphase_layers;
	std::unique_ptr<RagdollObjectVsBroadPhase> object_vs_broadphase;
	std::unique_ptr<RagdollObjectPairs> object_pairs;
	std::unique_ptr<JPH::TempAllocatorImpl> temp;
	std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
	std::unique_ptr<JPH::PhysicsSystem> system;
	std::vector<JPH::BodyID> statics;
	JPH::Ref<JPH::RagdollSettings> settings;
	std::vector<uint32_t> order;
	std::vector<uint32_t> parents;

	struct Instance {
		JPH::Ref<JPH::Ragdoll> ragdoll;
		bool frozen = false;
		std::vector<RagdollBodyPose> frozen_pose;
	};
	std::unordered_map<uint32_t, Instance> ragdolls;
	JPH::CollisionGroup::GroupID next_group = 1;

	void read_pose(const Instance &p_instance, std::span<RagdollBodyPose> r_bodies) const;
};

RagdollWorld::RagdollWorld(std::unique_ptr<Impl> p_impl) : impl_(std::move(p_impl)) {}

RagdollWorld::~RagdollWorld() {
	if (impl_ == nullptr) {
		return;
	}
	for (auto &[id, r] : impl_->ragdolls) {
		if (!r.frozen) {
			r.ragdoll->RemoveFromPhysicsSystem(false);
		}
	}
	impl_->ragdolls.clear();
}

core::Result<std::unique_ptr<RagdollWorld>> RagdollWorld::create(std::shared_ptr<const StaticWorldShapes> p_shapes,
		std::shared_ptr<const RagdollDef> p_def, const RagdollWorldOptions &p_options) {
	if (p_shapes == nullptr || p_def == nullptr) {
		return core::make_error("ragdoll world: no static shapes or no ragdoll");
	}
	const RagdollDef &def = *p_def;
	const size_t n = def.bodies.size();
	if (n == 0 || def.joints.size() + 1 != n) {
		return core::make_error("ragdoll world: " + std::to_string(n) + " bodies and " +
				std::to_string(def.joints.size()) + " joints are not one tree");
	}
	if (p_options.max_ragdolls == 0 || p_options.max_ragdolls > 1024 || !(p_options.gravity >= 0.0f)) {
		return core::make_error("ragdoll world: options out of range");
	}
	auto impl = std::make_unique<Impl>();
	Impl &w = *impl;
	w.parents.assign(n, kNoParentBody);
	std::vector<int32_t> joint_of(n, -1);
	for (size_t j = 0; j < def.joints.size(); ++j) {
		const RagdollJointDef &joint = def.joints[j];
		if (w.parents[joint.child] != kNoParentBody) {
			return core::make_error("ragdoll world: body " + def.bodies[joint.child].name + " has two parents");
		}
		w.parents[joint.child] = joint.parent;
		joint_of[joint.child] = int32_t(j);
	}
	std::vector<bool> placed(n, false);
	while (w.order.size() < n) {
		const size_t before = w.order.size();
		for (uint32_t b = 0; b < n; ++b) {
			if (!placed[b] && (w.parents[b] == kNoParentBody || placed[w.parents[b]])) {
				placed[b] = true;
				w.order.push_back(b);
			}
		}
		if (w.order.size() == before) {
			return core::make_error("ragdoll world: the joints form a cycle");
		}
	}
	if (w.parents[w.order[0]] != kNoParentBody || std::count(w.parents.begin(), w.parents.end(), kNoParentBody) != 1) {
		return core::make_error("ragdoll world: not a single root");
	}
	std::vector<int32_t> skeleton_index(n, -1);
	JPH::Ref<JPH::Skeleton> skeleton = new JPH::Skeleton();
	w.settings = new JPH::RagdollSettings();
	w.settings->mSkeleton = skeleton;
	for (size_t k = 0; k < n; ++k) {
		const uint32_t b = w.order[k];
		const RagdollBodyDef &body = def.bodies[b];
		skeleton_index[b] = int32_t(k);
		skeleton->AddJoint(
				JPH::String(body.bone.c_str()), w.parents[b] == kNoParentBody ? -1 : skeleton_index[w.parents[b]]);
		core::Result<JPH::RefConst<JPH::Shape>> shape = body_shape(body);
		if (!shape) {
			return shape.error();
		}
		JPH::RagdollSettings::Part part;
		part.SetShape(shape->GetPtr());
		part.mPosition = JPH::RVec3(to_jolt(body.rest.trans));
		part.mRotation = quat_of(body.rest);
		part.mMotionType = JPH::EMotionType::Dynamic;
		part.mObjectLayer = kRagdollObjectLayer;
		part.mFriction = body.friction;
		part.mRestitution = body.restitution;
		part.mLinearDamping = body.linear_damping;
		part.mAngularDamping = body.angular_damping;
		part.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
		part.mMassPropertiesOverride.mMass = body.mass;
		const core::Vec3 &inv = body.inverse_inertia;
		if (!(inv.x > 0.0f && inv.y > 0.0f && inv.z > 0.0f)) {
			return core::make_error("ragdoll world: body " + body.name + " has no inverse inertia");
		}
		part.mMassPropertiesOverride.mInertia = JPH::Mat44::sScale(JPH::Vec3(1.0f / inv.x, 1.0f / inv.y, 1.0f / inv.z));
		if (joint_of[b] >= 0) {
			part.mToParent = joint_settings(def, def.joints[size_t(joint_of[b])]);
		}
		w.settings->mParts.push_back(part);
	}
	skeleton->CalculateParentJointIndices();
	w.settings->DisableParentChildCollisions();
	w.settings->CalculateBodyIndexToConstraintIndex();
	w.settings->CalculateConstraintIndexToBodyIdxPair();

	const std::vector<StaticBodySpec> specs = static_body_specs(p_shapes->impl());
	const uint64_t max_bodies = uint64_t(specs.size()) + uint64_t(p_options.max_ragdolls) * n;
	if (max_bodies > (1u << 22)) {
		return core::make_error("ragdoll world: " + std::to_string(max_bodies) + " bodies exceed Jolt's limit");
	}
	w.runtime = jolt_runtime_acquire();
	w.shapes = std::move(p_shapes);
	w.def = std::move(p_def);
	w.options = p_options;
	w.broadphase_layers = std::make_unique<RagdollBroadPhaseLayers>();
	w.object_vs_broadphase = std::make_unique<RagdollObjectVsBroadPhase>();
	w.object_pairs = std::make_unique<RagdollObjectPairs>();
	w.temp = std::make_unique<JPH::TempAllocatorImpl>(p_options.temp_allocator_bytes);
	w.jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
	w.system = std::make_unique<JPH::PhysicsSystem>();
	w.system->Init(JPH::uint(max_bodies), 0, p_options.max_body_pairs, p_options.max_contact_constraints,
			*w.broadphase_layers, *w.object_vs_broadphase, *w.object_pairs);
	w.system->SetGravity(JPH::Vec3(0.0f, -p_options.gravity, 0.0f));
	JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
	w.statics.reserve(specs.size());
	for (const StaticBodySpec &spec : specs) {
		JPH::BodyCreationSettings body(spec.shape, JPH::RVec3(spec.position), spec.rotation, JPH::EMotionType::Static,
				JPH::ObjectLayer(spec.layer));
		body.mUserData = spec.user;
		JPH::Body *created = bi.CreateBody(body);
		if (created == nullptr) {
			return core::make_error("ragdoll world: Jolt refused static body " + std::to_string(w.statics.size()));
		}
		w.statics.push_back(created->GetID());
	}
	if (!w.statics.empty()) {
		const JPH::BodyInterface::AddState state = bi.AddBodiesPrepare(w.statics.data(), int(w.statics.size()));
		bi.AddBodiesFinalize(w.statics.data(), int(w.statics.size()), state, JPH::EActivation::DontActivate);
	}
	w.system->OptimizeBroadPhase();
	return std::unique_ptr<RagdollWorld>(new RagdollWorld(std::move(impl)));
}

const RagdollDef &RagdollWorld::def() const {
	return *impl_->def;
}

size_t RagdollWorld::body_count() const {
	return impl_->def->bodies.size();
}

std::span<const uint32_t> RagdollWorld::parents() const {
	return impl_->parents;
}

core::Status RagdollWorld::add(uint32_t p_id, std::span<const RagdollBodyPose> p_bodies,
		std::span<const Vec3> p_linear_velocities, std::span<const Vec3> p_angular_velocities) {
	Impl &w = *impl_;
	const size_t n = body_count();
	if (p_bodies.size() != n || (!p_linear_velocities.empty() && p_linear_velocities.size() != n) ||
			(!p_angular_velocities.empty() && p_angular_velocities.size() != n)) {
		return core::make_error("ragdoll world: pose of " + std::to_string(p_bodies.size()) +
				" bodies for a ragdoll of " + std::to_string(n));
	}
	if (w.ragdolls.contains(p_id)) {
		return core::make_error("ragdoll world: ragdoll " + std::to_string(p_id) + " exists");
	}
	if (w.ragdolls.size() >= w.options.max_ragdolls) {
		return core::make_error("ragdoll world: " + std::to_string(w.options.max_ragdolls) + " ragdolls in use");
	}
	std::vector<JPH::Mat44> joints(n);
	for (size_t k = 0; k < n; ++k) {
		const RagdollBodyPose &p = p_bodies[w.order[k]];
		const JPH::Quat q = to_jolt(p.rotation);
		if (!q.IsNormalized(1.0e-3f) || !std::isfinite(p.position.x) || !std::isfinite(p.position.y) ||
				!std::isfinite(p.position.z)) {
			return core::make_error(
					"ragdoll world: body " + std::to_string(w.order[k]) + " pose is not finite or not a rotation");
		}
		joints[k] = JPH::Mat44::sRotationTranslation(q.Normalized(), to_jolt(p.position));
	}
	JPH::Ref<JPH::Ragdoll> ragdoll = w.settings->CreateRagdoll(w.next_group++, uint64_t(p_id), w.system.get());
	if (ragdoll == nullptr) {
		return core::make_error("ragdoll world: Jolt is out of bodies");
	}
	ragdoll->SetPose(JPH::RVec3::sZero(), joints.data(), false);
	JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
	const JPH::Array<JPH::BodyID> &ids = ragdoll->GetBodyIDs();
	for (size_t k = 0; k < n; ++k) {
		const uint32_t b = w.order[k];
		const JPH::Vec3 v = p_linear_velocities.empty() ? JPH::Vec3::sZero() : to_jolt(p_linear_velocities[b]);
		const JPH::Vec3 a = p_angular_velocities.empty() ? JPH::Vec3::sZero() : to_jolt(p_angular_velocities[b]);
		bi.SetLinearAndAngularVelocity(
				ids[JPH::uint(k)], v.IsNaN() ? JPH::Vec3::sZero() : v, a.IsNaN() ? JPH::Vec3::sZero() : a);
	}
	ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate, false);
	Impl::Instance instance;
	instance.ragdoll = ragdoll;
	w.ragdolls.emplace(p_id, std::move(instance));
	return core::Status::success();
}

bool RagdollWorld::has(uint32_t p_id) const {
	return impl_->ragdolls.contains(p_id);
}

void RagdollWorld::remove(uint32_t p_id) {
	auto it = impl_->ragdolls.find(p_id);
	if (it == impl_->ragdolls.end()) {
		return;
	}
	if (!it->second.frozen) {
		it->second.ragdoll->RemoveFromPhysicsSystem(false);
	}
	impl_->ragdolls.erase(it);
}

void RagdollWorld::freeze(uint32_t p_id) {
	auto it = impl_->ragdolls.find(p_id);
	if (it == impl_->ragdolls.end() || it->second.frozen) {
		return;
	}
	Impl::Instance &r = it->second;
	r.frozen_pose.resize(body_count());
	impl_->read_pose(r, r.frozen_pose);
	r.ragdoll->RemoveFromPhysicsSystem(false);
	r.frozen = true;
}

bool RagdollWorld::frozen(uint32_t p_id) const {
	auto it = impl_->ragdolls.find(p_id);
	return it != impl_->ragdolls.end() && it->second.frozen;
}

size_t RagdollWorld::ragdoll_count() const {
	return impl_->ragdolls.size();
}

core::Status RagdollWorld::step(float p_dt) {
	if (!std::isfinite(p_dt) || p_dt < 0.0f) {
		return core::make_error("ragdoll world: step of " + std::to_string(p_dt) + " s");
	}
	if (p_dt == 0.0f) {
		return core::Status::success();
	}
	const int steps = int(std::clamp<float>(std::ceil(p_dt / kRagdollStepSeconds), 1.0f, float(kMaxRagdollSubSteps)));
	const JPH::EPhysicsUpdateError error = impl_->system->Update(p_dt, steps, impl_->temp.get(), impl_->jobs.get());
	if (error != JPH::EPhysicsUpdateError::None) {
		return core::make_error("ragdoll world: Jolt update error " + std::to_string(uint32_t(error)));
	}
	return core::Status::success();
}

void RagdollWorld::Impl::read_pose(const Instance &p_instance, std::span<RagdollBodyPose> r_bodies) const {
	const JPH::BodyInterface &bi = system->GetBodyInterfaceNoLock();
	const JPH::Array<JPH::BodyID> &ids = p_instance.ragdoll->GetBodyIDs();
	for (size_t k = 0; k < order.size(); ++k) {
		JPH::RVec3 position;
		JPH::Quat rotation;
		bi.GetPositionAndRotation(ids[JPH::uint(k)], position, rotation);
		RagdollBodyPose &out = r_bodies[order[k]];
		out.position = from_jolt(JPH::Vec3(position));
		out.rotation = { rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW() };
	}
}

bool RagdollWorld::pose(uint32_t p_id, std::span<RagdollBodyPose> r_bodies) const {
	auto it = impl_->ragdolls.find(p_id);
	if (it == impl_->ragdolls.end() || r_bodies.size() != body_count()) {
		return false;
	}
	if (it->second.frozen) {
		std::copy(it->second.frozen_pose.begin(), it->second.frozen_pose.end(), r_bodies.begin());
	} else {
		impl_->read_pose(it->second, r_bodies);
	}
	return true;
}

float RagdollWorld::max_linear_speed(uint32_t p_id) const {
	auto it = impl_->ragdolls.find(p_id);
	if (it == impl_->ragdolls.end() || it->second.frozen) {
		return 0.0f;
	}
	const JPH::BodyInterface &bi = impl_->system->GetBodyInterfaceNoLock();
	float m = 0.0f;
	for (const JPH::BodyID &id : it->second.ragdoll->GetBodyIDs()) {
		m = std::max(m, bi.GetLinearVelocity(id).Length());
	}
	return m;
}

float RagdollWorld::max_angular_speed(uint32_t p_id) const {
	auto it = impl_->ragdolls.find(p_id);
	if (it == impl_->ragdolls.end() || it->second.frozen) {
		return 0.0f;
	}
	const JPH::BodyInterface &bi = impl_->system->GetBodyInterfaceNoLock();
	float m = 0.0f;
	for (const JPH::BodyID &id : it->second.ragdoll->GetBodyIDs()) {
		m = std::max(m, bi.GetAngularVelocity(id).Length());
	}
	return m;
}

}
