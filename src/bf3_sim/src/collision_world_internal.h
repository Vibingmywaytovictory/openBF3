#pragma once
// bf3_sim internal (not a public header; Jolt stays PRIVATE to bf3_sim): CollisionWorld::Impl, the seam between
// the world track and the motor track (docs/design/m1.md §5.6, §15.1). T1 phase 1 defines the member functions in
// collision_world.cpp; SoldierMotor (T2, soldier_motor.cpp) builds its CharacterVirtuals on `system` and passes
// the filters below to RefreshContacts / ExtendedUpdate. T2 relies on exactly these members; anything else in
// here is T1's to change.
//
// Layers: one object layer per StaticLayer (0 Collision, 1 Raycast, 2 Character), plus kQueryObjectLayer for the
// character and the shape casts (it is never a body's layer). One broadphase layer, "static". Every body is static
// and created in (tile, layer) order, then OptimizeBroadPhase (docs §5.3), so two worlds over the same shapes are
// identical.
//
// Threads: owner thread only (collision_world.h). Destruction order: the members are declared so that the system
// dies before the layer interfaces and filters it references, and `runtime` last.

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

inline constexpr JPH::ObjectLayer kQueryObjectLayer = 3; // the moving query shape (character, sweeps)
inline constexpr uint32_t kObjectLayerCount = 4;

struct CollisionWorld::Impl {
	// Keeps the Jolt runtime (RegisterTypes, factory, allocator hooks; jolt_runtime.h, T1) alive while any
	// system or shape of this world exists. Declared first: destroyed last.
	std::shared_ptr<void> runtime;
	std::shared_ptr<const StaticWorldShapes> shapes;

	std::unique_ptr<JPH::BroadPhaseLayerInterface> broadphase_layers;
	std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilter> object_vs_broadphase;
	std::unique_ptr<JPH::ObjectLayerPairFilter> object_pairs;
	// The query filters for a query_mask:: value (0..7); index = mask. Built once in create().
	std::vector<std::unique_ptr<JPH::BroadPhaseLayerFilter>> broadphase_filters;
	std::vector<std::unique_ptr<JPH::ObjectLayerFilter>> object_filters;
	JPH::BodyFilter body_filter; // accepts every body (all static)
	JPH::ShapeFilter shape_filter; // accepts every sub-shape

	std::unique_ptr<JPH::TempAllocatorImpl> temp; // CollisionWorldOptions::temp_allocator_bytes
	std::unique_ptr<JPH::PhysicsSystem> system; // static bodies only; never stepped in M1 (declared last: dies first)
	std::vector<JPH::BodyID> bodies; // (tile, layer) order

	// The filters for p_mask (query_mask::kMovement for the motor). p_mask must be < 8.
	const JPH::BroadPhaseLayerFilter &broadphase_filter(uint32_t p_mask) const;
	const JPH::ObjectLayerFilter &object_filter(uint32_t p_mask) const;
	// The tile, layer and SurfaceTag of a hit (body user data = tile * 4 + layer; the sub-shape's triangle user data
	// indexes the tile's tag table), as StaticHit carries them.
	void describe_hit(const JPH::BodyID &p_body, const JPH::SubShapeID &p_sub_shape, StaticHit &r_hit) const;
};

} // namespace bf3::sim
