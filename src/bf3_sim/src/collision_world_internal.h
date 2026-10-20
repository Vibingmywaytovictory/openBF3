#pragma once

#include "bf3/sim/collision_world.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::sim {

inline constexpr JPH::ObjectLayer kQueryObjectLayer = 3;
inline constexpr uint32_t kObjectLayerCount = 4;

struct CollisionWorld::Impl {

	std::shared_ptr<void> runtime;
	std::shared_ptr<const StaticWorldShapes> shapes;

	std::unique_ptr<JPH::BroadPhaseLayerInterface> broadphase_layers;
	std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilter> object_vs_broadphase;
	std::unique_ptr<JPH::ObjectLayerPairFilter> object_pairs;

	std::vector<std::unique_ptr<JPH::BroadPhaseLayerFilter>> broadphase_filters;
	std::vector<std::unique_ptr<JPH::ObjectLayerFilter>> object_filters;
	JPH::BodyFilter body_filter;
	JPH::ShapeFilter shape_filter;

	std::unique_ptr<JPH::TempAllocatorImpl> temp;
	std::unique_ptr<JPH::PhysicsSystem> system;
	std::vector<JPH::BodyID> bodies;

	const JPH::BroadPhaseLayerFilter &broadphase_filter(uint32_t p_mask) const;
	const JPH::ObjectLayerFilter &object_filter(uint32_t p_mask) const;

	void describe_hit(const JPH::BodyID &p_body, const JPH::SubShapeID &p_sub_shape, StaticHit &r_hit) const;
};

}
