#include "collision_world_internal.h"

#include "bf3/core/util/finite.h"

#include "jolt_runtime.h"
#include "static_world_internal.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::sim {

namespace {

constexpr JPH::BroadPhaseLayer kStaticBroadPhase(0);
constexpr uint32_t kMaskCount = 8;

class StaticBroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
	JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override { return kStaticBroadPhase; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "STATIC"; }
#endif
};

class StaticObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer p_layer, JPH::BroadPhaseLayer) const override { return p_layer >= kStaticLayerCount; }
};

class StaticObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
	bool ShouldCollide(JPH::ObjectLayer p_a, JPH::ObjectLayer p_b) const override {
		return p_a >= kStaticLayerCount || p_b >= kStaticLayerCount;
	}
};

class MaskBroadPhaseFilter final : public JPH::BroadPhaseLayerFilter {
public:
	explicit MaskBroadPhaseFilter(uint32_t p_mask) :
			mask_(p_mask) {}
	bool ShouldCollide(JPH::BroadPhaseLayer) const override {
		++queries_;
		return mask_ != 0;
	}
	uint64_t queries() const { return queries_; }

private:
	uint32_t mask_;
	mutable uint64_t queries_ = 0;
};

class MaskObjectFilter final : public JPH::ObjectLayerFilter {
public:
	explicit MaskObjectFilter(uint32_t p_mask) :
			mask_(p_mask) {}
	bool ShouldCollide(JPH::ObjectLayer p_layer) const override { return p_layer < kStaticLayerCount && ((mask_ >> p_layer) & 1u) != 0; }

private:
	uint32_t mask_;
};

class DeeperThanCollector final : public JPH::CollideShapeCollector {
public:
	explicit DeeperThanCollector(float p_tolerance) :
			tolerance_(p_tolerance) {}
	void AddHit(const JPH::CollideShapeResult &p_result) override {
		if (p_result.mPenetrationDepth > tolerance_) {
			hit_ = true;
			ForceEarlyOut();
		}
	}
	bool hit() const { return hit_; }

private:
	float tolerance_;
	bool hit_ = false;
};

using bf3::core::finite;

JPH::Vec3 to_jolt(const Vec3 &p_v) {
	return JPH::Vec3(p_v[0], p_v[1], p_v[2]);
}

Vec3 from_jolt(JPH::Vec3Arg p_v) {
	return { p_v.GetX(), p_v.GetY(), p_v.GetZ() };
}

JPH::RefConst<JPH::Shape> upright_capsule(float p_radius, float p_height) {
	if (!std::isfinite(p_radius) || !std::isfinite(p_height) || p_radius <= 0.0f || p_height < 2.0f * p_radius) {
		return nullptr;
	}
	const float half_cylinder = 0.5f * p_height - p_radius;
	if (half_cylinder <= 1e-6f) {
		return new JPH::SphereShape(p_radius);
	}
	return new JPH::CapsuleShape(half_cylinder, p_radius);
}

}

const JPH::BroadPhaseLayerFilter &CollisionWorld::Impl::broadphase_filter(uint32_t p_mask) const {
	return *broadphase_filters[p_mask & (kMaskCount - 1)];
}

const JPH::ObjectLayerFilter &CollisionWorld::Impl::object_filter(uint32_t p_mask) const {
	return *object_filters[p_mask & (kMaskCount - 1)];
}

void CollisionWorld::Impl::describe_hit(const JPH::BodyID &p_body, const JPH::SubShapeID &p_sub_shape, StaticHit &r_hit) const {
	const uint64_t user = system->GetBodyInterfaceNoLock().GetUserData(p_body);
	const uint64_t tile = user >> 2;
	const uint32_t layer = uint32_t(user & 3u);
	if (layer == kStaticTerrainBodyLayer) {

		r_hit.tile = kNoStaticTile;
		r_hit.terrain = uint32_t(tile);
		r_hit.layer = StaticLayer::Collision;
		r_hit.tag = tile < shapes->impl().terrains.size() ? static_terrain_tag() : SurfaceTag();
		return;
	}
	r_hit.tile = uint32_t(tile);
	r_hit.layer = StaticLayer(layer < kStaticLayerCount ? layer : 0);
	r_hit.tag = SurfaceTag();
	const StaticWorldShapes::Impl &s = shapes->impl();
	if (tile >= s.tiles.size() || layer >= kStaticLayerCount) {
		return;
	}
	const JPH::Shape *shape = s.tiles[tile].shapes[layer].GetPtr();
	if (shape == nullptr || shape->GetSubType() != JPH::EShapeSubType::Mesh) {
		return;
	}
	const uint32_t index = static_cast<const JPH::MeshShape *>(shape)->GetTriangleUserData(p_sub_shape);
	r_hit.tag = shapes->tag(tile, index);
}

namespace {

JPH::Vec3 face_normal(const CollisionWorld::Impl &p_impl, const JPH::BodyID &p_body, const JPH::SubShapeID &p_sub_shape, JPH::Vec3Arg p_towards) {
	const JPH::BodyInterface &bi = p_impl.system->GetBodyInterfaceNoLock();
	const uint64_t user = bi.GetUserData(p_body);
	const uint64_t tile = user >> 2;
	const uint32_t layer = uint32_t(user & 3u);
	const StaticWorldShapes::Impl &s = p_impl.shapes->impl();
	JPH::Vec3 n;
	if (layer == kStaticTerrainBodyLayer) {
		const JPH::RefConst<JPH::Shape> shape = bi.GetShape(p_body);
		n = static_face_normal(s, user, shape.GetPtr(), p_sub_shape);
	} else if (tile >= s.tiles.size() || layer >= kStaticLayerCount || s.tiles[tile].shapes[layer] == nullptr) {
		return JPH::Vec3::sAxisY();
	} else {
		n = s.tiles[tile].shapes[layer]->GetSurfaceNormal(p_sub_shape, JPH::Vec3::sZero());
	}
	if (n.Dot(p_towards) < 0.0f) {
		n = -n;
	}
	return n;
}

}

CollisionWorld::CollisionWorld(std::unique_ptr<Impl> p_impl) :
		impl_(std::move(p_impl)) {}

CollisionWorld::~CollisionWorld() {
	Impl &w = *impl_;
	if (w.system) {
		JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
		if (!w.bodies.empty()) {
			bi.RemoveBodies(w.bodies.data(), int(w.bodies.size()));
			bi.DestroyBodies(w.bodies.data(), int(w.bodies.size()));
		}
	}
	w.bodies.clear();
	w.system.reset();
	w.temp.reset();
}

core::Result<std::unique_ptr<CollisionWorld>> CollisionWorld::create(std::shared_ptr<const StaticWorldShapes> p_shapes, const CollisionWorldOptions &p_options) {
	if (p_shapes == nullptr) {
		return core::make_error("collision world: no static shapes");
	}
	if (p_options.temp_allocator_bytes < (64u << 10)) {
		return core::make_error("collision world: temp_allocator_bytes " + std::to_string(p_options.temp_allocator_bytes) + " is below 64 KiB");
	}
	const std::vector<StaticBodySpec> specs = static_body_specs(p_shapes->impl());
	const size_t static_bodies = specs.size();
	const uint64_t max_bodies = uint64_t(static_bodies) + p_options.max_dynamic_bodies;
	if (max_bodies > (1u << 22)) {
		return core::make_error("collision world: " + std::to_string(max_bodies) + " bodies exceed Jolt's limit");
	}

	auto impl = std::make_unique<Impl>();
	Impl &w = *impl;
	w.runtime = jolt_runtime_acquire();
	w.shapes = std::move(p_shapes);
	w.broadphase_layers = std::make_unique<StaticBroadPhaseLayers>();
	w.object_vs_broadphase = std::make_unique<StaticObjectVsBroadPhase>();
	w.object_pairs = std::make_unique<StaticObjectPairs>();
	for (uint32_t mask = 0; mask < kMaskCount; ++mask) {
		w.broadphase_filters.push_back(std::make_unique<MaskBroadPhaseFilter>(mask));
		w.object_filters.push_back(std::make_unique<MaskObjectFilter>(mask));
	}
	w.temp = std::make_unique<JPH::TempAllocatorImpl>(p_options.temp_allocator_bytes);
	w.system = std::make_unique<JPH::PhysicsSystem>();
	const JPH::uint pairs = std::max<JPH::uint>(1024, p_options.max_dynamic_bodies * 4);
	w.system->Init(JPH::uint(max_bodies), 0, pairs, pairs, *w.broadphase_layers, *w.object_vs_broadphase, *w.object_pairs);

	JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
	w.bodies.reserve(static_bodies);
	for (const StaticBodySpec &spec : specs) {
		JPH::BodyCreationSettings settings(spec.shape, JPH::RVec3(spec.position), spec.rotation, JPH::EMotionType::Static, JPH::ObjectLayer(spec.layer));
		settings.mUserData = spec.user;
		JPH::Body *body = bi.CreateBody(settings);
		if (body == nullptr) {
			return core::make_error("collision world: Jolt refused body " + std::to_string(w.bodies.size()));
		}
		w.bodies.push_back(body->GetID());
	}
	if (!w.bodies.empty()) {
		const JPH::BodyInterface::AddState state = bi.AddBodiesPrepare(w.bodies.data(), int(w.bodies.size()));
		bi.AddBodiesFinalize(w.bodies.data(), int(w.bodies.size()), state, JPH::EActivation::DontActivate);
	}
	w.system->OptimizeBroadPhase();
	return std::unique_ptr<CollisionWorld>(new CollisionWorld(std::move(impl)));
}

uint64_t CollisionWorld::query_count() const {
	uint64_t total = 0;
	for (const std::unique_ptr<JPH::BroadPhaseLayerFilter> &f : impl_->broadphase_filters) {
		total += static_cast<const MaskBroadPhaseFilter *>(f.get())->queries();
	}
	return total;
}

const std::shared_ptr<const StaticWorldShapes> &CollisionWorld::shapes() const {
	return impl_->shapes;
}

bool CollisionWorld::ray(const Vec3 &p_origin, const Vec3 &p_delta, uint32_t p_mask, StaticHit *r_hit) const {
	const Impl &w = *impl_;
	if (!finite(p_origin) || !finite(p_delta)) {
		return false;
	}
	const JPH::Vec3 delta = to_jolt(p_delta);
	if (delta.LengthSq() <= 0.0f) {
		return false;
	}
	const JPH::RRayCast cast(JPH::RVec3(to_jolt(p_origin)), delta);
	JPH::RayCastSettings settings;
	settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
	JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
	w.system->GetNarrowPhaseQueryNoLock().CastRay(cast, settings, collector, w.broadphase_filter(p_mask), w.object_filter(p_mask), w.body_filter,
			w.shape_filter);
	if (!collector.HadHit()) {
		return false;
	}
	if (r_hit != nullptr) {
		const JPH::RayCastResult &hit = collector.mHit;
		*r_hit = StaticHit();
		w.describe_hit(hit.mBodyID, hit.mSubShapeID2, *r_hit);
		r_hit->fraction = hit.mFraction;
		r_hit->position = from_jolt(to_jolt(p_origin) + delta * hit.mFraction);
		r_hit->normal = from_jolt(face_normal(w, hit.mBodyID, hit.mSubShapeID2, -delta));
	}
	return true;
}

bool CollisionWorld::ray_any(const Vec3 &p_origin, const Vec3 &p_delta, uint32_t p_mask) const {
	const Impl &w = *impl_;
	if (!finite(p_origin) || !finite(p_delta)) {
		return false;
	}
	const JPH::Vec3 delta = to_jolt(p_delta);
	if (delta.LengthSq() <= 0.0f) {
		return false;
	}
	const JPH::RRayCast cast(JPH::RVec3(to_jolt(p_origin)), delta);
	JPH::RayCastSettings settings;
	settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
	JPH::AnyHitCollisionCollector<JPH::CastRayCollector> collector;
	w.system->GetNarrowPhaseQueryNoLock().CastRay(cast, settings, collector, w.broadphase_filter(p_mask), w.object_filter(p_mask), w.body_filter,
			w.shape_filter);
	return collector.HadHit();
}

bool CollisionWorld::sweep_capsule(const Vec3 &p_feet, float p_radius, float p_height, const Vec3 &p_delta, uint32_t p_mask, StaticHit *r_hit) const {
	const Impl &w = *impl_;
	if (!finite(p_feet) || !finite(p_delta)) {
		return false;
	}
	const JPH::RefConst<JPH::Shape> shape = upright_capsule(p_radius, p_height);
	if (shape == nullptr) {
		return false;
	}
	const JPH::Vec3 center = to_jolt(p_feet) + JPH::Vec3(0.0f, 0.5f * p_height, 0.0f);
	const JPH::Vec3 delta = to_jolt(p_delta);
	if (delta.LengthSq() <= 1e-12f) {

		JPH::CollideShapeSettings settings;
		settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
		JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
		w.system->GetNarrowPhaseQueryNoLock().CollideShape(shape, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(JPH::RVec3(center)), settings,
				JPH::RVec3(center), collector, w.broadphase_filter(p_mask), w.object_filter(p_mask), w.body_filter, w.shape_filter);
		if (!collector.HadHit()) {
			return false;
		}
		if (r_hit != nullptr) {
			const JPH::CollideShapeResult &hit = collector.mHit;
			*r_hit = StaticHit();
			w.describe_hit(hit.mBodyID2, hit.mSubShapeID2, *r_hit);
			r_hit->fraction = 0.0f;
			r_hit->position = from_jolt(center + hit.mContactPointOn2);
			JPH::Vec3 towards = -hit.mPenetrationAxis;
			if (towards.LengthSq() <= 1e-12f) {
				towards = JPH::Vec3::sAxisY();
			}
			r_hit->normal = from_jolt(face_normal(w, hit.mBodyID2, hit.mSubShapeID2, towards));
		}
		return true;
	}
	const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(shape, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(JPH::RVec3(center)), delta);
	JPH::ShapeCastSettings settings;
	settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
	settings.mReturnDeepestPoint = true;
	JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;

	w.system->GetNarrowPhaseQueryNoLock().CastShape(cast, settings, JPH::RVec3(center), collector, w.broadphase_filter(p_mask), w.object_filter(p_mask),
			w.body_filter, w.shape_filter);
	if (!collector.HadHit()) {
		return false;
	}
	if (r_hit != nullptr) {
		const JPH::ShapeCastResult &hit = collector.mHit;
		*r_hit = StaticHit();
		w.describe_hit(hit.mBodyID2, hit.mSubShapeID2, *r_hit);
		r_hit->fraction = hit.mFraction;
		r_hit->position = from_jolt(center + hit.mContactPointOn2);

		JPH::Vec3 towards = -hit.mPenetrationAxis;
		if (towards.LengthSq() <= 1e-12f) {
			towards = -delta;
		}
		r_hit->normal = from_jolt(face_normal(w, hit.mBodyID2, hit.mSubShapeID2, towards));
	}
	return true;
}

bool CollisionWorld::overlap_capsule(const Vec3 &p_feet, float p_radius, float p_height, uint32_t p_mask, float p_tolerance) const {
	const Impl &w = *impl_;
	if (!finite(p_feet) || !std::isfinite(p_tolerance)) {
		return false;
	}
	const JPH::RefConst<JPH::Shape> shape = upright_capsule(p_radius, p_height);
	if (shape == nullptr) {
		return false;
	}
	const JPH::Vec3 center = to_jolt(p_feet) + JPH::Vec3(0.0f, 0.5f * p_height, 0.0f);
	JPH::CollideShapeSettings settings;
	settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
	DeeperThanCollector collector(std::max(p_tolerance, 0.0f));
	w.system->GetNarrowPhaseQueryNoLock().CollideShape(shape, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(JPH::RVec3(center)), settings, JPH::RVec3(center),
			collector, w.broadphase_filter(p_mask), w.object_filter(p_mask), w.body_filter, w.shape_filter);
	return collector.hit();
}

bool CollisionWorld::overlap_box(const Vec3 &p_center, const Vec3 &p_half, float p_yaw, uint32_t p_mask, float p_tolerance) const {
	const Impl &w = *impl_;
	if (!finite(p_center) || !finite(p_half) || !std::isfinite(p_yaw) || !std::isfinite(p_tolerance)) {
		return false;
	}
	const JPH::Vec3 half = to_jolt(p_half);
	if (half.ReduceMin() <= 0.0f) {
		return false;
	}
	const JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(half, std::min(JPH::cDefaultConvexRadius, 0.5f * half.ReduceMin()));
	const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(), p_yaw), JPH::RVec3(to_jolt(p_center)));
	JPH::CollideShapeSettings settings;
	settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
	DeeperThanCollector collector(std::max(p_tolerance, 0.0f));
	w.system->GetNarrowPhaseQueryNoLock().CollideShape(shape, JPH::Vec3::sOne(), transform, settings, JPH::RVec3(to_jolt(p_center)), collector,
			w.broadphase_filter(p_mask), w.object_filter(p_mask), w.body_filter, w.shape_filter);
	return collector.hit();
}

bool CollisionWorld::ground_below(const Vec3 &p_from, float p_max_drop, uint32_t p_mask, StaticHit *r_hit) const {

	if (!std::isfinite(p_max_drop) || p_max_drop <= 0.0f) {
		return false;
	}
	return ray(p_from, { 0.0f, -p_max_drop, 0.0f }, p_mask, r_hit);
}

}
