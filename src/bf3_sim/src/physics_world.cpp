#include "bf3/sim/physics_world.h"

#include "jolt_runtime.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cstring>

namespace bf3::sim {

namespace {

namespace Layers {
constexpr JPH::ObjectLayer kNonMoving = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::ObjectLayer kCount = 2;
}

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer kNonMoving(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr JPH::uint kCount = 2;
}

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
	JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kCount; }
	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer p_layer) const override {
		return p_layer == Layers::kNonMoving ? BroadPhaseLayers::kNonMoving : BroadPhaseLayers::kMoving;
	}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer p_layer) const override {
		return p_layer == BroadPhaseLayers::kNonMoving ? "NON_MOVING" : "MOVING";
	}
#endif
};

class ObjectVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer p_layer, JPH::BroadPhaseLayer p_bp) const override {
		return p_layer == Layers::kMoving || p_bp == BroadPhaseLayers::kMoving;
	}
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer p_a, JPH::ObjectLayer p_b) const override {
		return p_a == Layers::kMoving || p_b == Layers::kMoving;
	}
};

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void fnv_float(uint64_t &r_h, float p_v) {
	uint32_t bits;
	std::memcpy(&bits, &p_v, sizeof(bits));
	for (int i = 0; i < 4; ++i) {
		r_h ^= (bits >> (i * 8)) & 0xffu;
		r_h *= kFnvPrime;
	}
}

}

struct PhysicsWorld::Impl {
	BPLayerInterface bp_layers;
	ObjectVsBPFilter obj_vs_bp;
	ObjectPairFilter obj_pairs;
	std::unique_ptr<JPH::TempAllocatorImpl> temp;
	std::unique_ptr<JPH::JobSystem> jobs;
	std::unique_ptr<JPH::PhysicsSystem> system;
	std::vector<JPH::BodyID> dynamic_ids;
	std::vector<BodyTransform> dynamic_shapes;
	JPH::BodyID ground;
	uint64_t tick = 0;

	std::shared_ptr<void> runtime;
};

PhysicsWorld::PhysicsWorld() :
		impl_(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld() {
	shutdown();
}

bool PhysicsWorld::is_initialized() const {
	return impl_->system != nullptr;
}

bool PhysicsWorld::init(const WorldConfig &p_config) {
	shutdown();
	Impl &w = *impl_;
	w.runtime = jolt_runtime_acquire();

	w.temp = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
	if (p_config.job_threads <= 0) {
		w.jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
	} else {
		w.jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, p_config.job_threads);
	}
	w.system = std::make_unique<JPH::PhysicsSystem>();
	w.system->Init(p_config.max_bodies, 0, p_config.max_bodies, p_config.max_bodies, w.bp_layers, w.obj_vs_bp, w.obj_pairs);

	JPH::BodyInterface &bi = w.system->GetBodyInterface();

	JPH::BodyCreationSettings ground(new JPH::BoxShape(JPH::Vec3(30.0f, 0.5f, 30.0f)), JPH::RVec3(0.0f, -0.5f, 0.0f), JPH::Quat::sIdentity(),
			JPH::EMotionType::Static, Layers::kNonMoving);
	w.ground = bi.CreateAndAddBody(ground, JPH::EActivation::DontActivate);

	for (int i = 0; i < p_config.dynamic_bodies; ++i) {
		BodyTransform meta;
		meta.shape = static_cast<uint8_t>(i % 2);
		meta.half_extent = 0.5f;
		JPH::RefConst<JPH::Shape> shape = meta.shape == 0 ? JPH::RefConst<JPH::Shape>(new JPH::BoxShape(JPH::Vec3::sReplicate(0.5f)))
														 : JPH::RefConst<JPH::Shape>(new JPH::SphereShape(0.5f));
		const float x = 0.35f * static_cast<float>((i % 3) - 1);
		const float z = 0.25f * static_cast<float>(((i / 3) % 3) - 1);
		const float y = 2.0f + 1.25f * static_cast<float>(i);
		const JPH::Quat rot = JPH::Quat::sRotation(JPH::Vec3(0.3f, 1.0f, 0.2f).Normalized(), 0.37f * static_cast<float>(i));
		JPH::BodyCreationSettings s(shape, JPH::RVec3(x, y, z), rot, JPH::EMotionType::Dynamic, Layers::kMoving);
		s.mRestitution = 0.2f;
		s.mFriction = 0.6f;
		s.mAngularDamping = meta.shape == 1 ? 0.8f : 0.05f;
		w.dynamic_ids.push_back(bi.CreateAndAddBody(s, JPH::EActivation::Activate));
		w.dynamic_shapes.push_back(meta);
	}
	w.system->OptimizeBroadPhase();
	w.tick = 0;
	return true;
}

void PhysicsWorld::shutdown() {
	Impl &w = *impl_;
	if (w.system) {
		JPH::BodyInterface &bi = w.system->GetBodyInterface();
		for (const JPH::BodyID &id : w.dynamic_ids) {
			bi.RemoveBody(id);
			bi.DestroyBody(id);
		}
		if (!w.ground.IsInvalid()) {
			bi.RemoveBody(w.ground);
			bi.DestroyBody(w.ground);
		}
	}
	w.dynamic_ids.clear();
	w.dynamic_shapes.clear();
	w.ground = JPH::BodyID();
	w.system.reset();
	w.jobs.reset();
	w.temp.reset();
	w.tick = 0;
	w.runtime.reset();
}

bool PhysicsWorld::step() {
	Impl &w = *impl_;
	if (!w.system) {
		return false;
	}
	const JPH::EPhysicsUpdateError err = w.system->Update(kStepSeconds, 1, w.temp.get(), w.jobs.get());
	w.tick++;
	return err == JPH::EPhysicsUpdateError::None;
}

uint64_t PhysicsWorld::tick() const {
	return impl_->tick;
}

uint64_t PhysicsWorld::state_hash() const {
	const Impl &w = *impl_;
	uint64_t h = kFnvOffset;
	if (!w.system) {
		return h;
	}
	const JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
	for (const JPH::BodyID &id : w.dynamic_ids) {
		const JPH::RVec3 p = bi.GetPosition(id);
		const JPH::Quat q = bi.GetRotation(id);
		const JPH::Vec3 v = bi.GetLinearVelocity(id);
		const JPH::Vec3 a = bi.GetAngularVelocity(id);
		for (float f : { float(p.GetX()), float(p.GetY()), float(p.GetZ()), q.GetX(), q.GetY(), q.GetZ(), q.GetW(), v.GetX(), v.GetY(),
					 v.GetZ(), a.GetX(), a.GetY(), a.GetZ() }) {
			fnv_float(h, f);
		}
	}
	return h;
}

int PhysicsWorld::dynamic_body_count() const {
	return static_cast<int>(impl_->dynamic_ids.size());
}

int PhysicsWorld::active_body_count() const {
	return impl_->system ? static_cast<int>(impl_->system->GetNumActiveBodies(JPH::EBodyType::RigidBody)) : 0;
}

float PhysicsWorld::lowest_dynamic_y() const {
	const Impl &w = *impl_;
	if (!w.system || w.dynamic_ids.empty()) {
		return 0.0f;
	}
	const JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
	float lowest = 1e30f;
	for (const JPH::BodyID &id : w.dynamic_ids) {
		lowest = std::min(lowest, float(bi.GetPosition(id).GetY()));
	}
	return lowest;
}

void PhysicsWorld::get_dynamic_transforms(std::vector<BodyTransform> &r_out) const {
	const Impl &w = *impl_;
	r_out.resize(w.dynamic_ids.size());
	if (!w.system) {
		return;
	}
	const JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
	for (size_t i = 0; i < w.dynamic_ids.size(); ++i) {
		BodyTransform &t = r_out[i];
		t = w.dynamic_shapes[i];
		JPH::RVec3 p;
		JPH::Quat q;
		bi.GetPositionAndRotation(w.dynamic_ids[i], p, q);
		t.position[0] = float(p.GetX());
		t.position[1] = float(p.GetY());
		t.position[2] = float(p.GetZ());
		t.rotation[0] = q.GetX();
		t.rotation[1] = q.GetY();
		t.rotation[2] = q.GetZ();
		t.rotation[3] = q.GetW();
	}
}

const char *PhysicsWorld::jolt_version() {
#define BF3_STR2(x) #x
#define BF3_STR(x) BF3_STR2(x)
	return BF3_STR(JPH_VERSION_MAJOR) "." BF3_STR(JPH_VERSION_MINOR) "." BF3_STR(JPH_VERSION_PATCH);
#undef BF3_STR
#undef BF3_STR2
}

}
