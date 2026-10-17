#pragma once
// CollisionWorld: one owner's view of the static world (plan §3.1 "ICollisionWorld {sweepCapsule, overlapCapsule,
// ray}", D5, D6; docs/design/m1.md §5.6). Each owner (the server thread, the client prediction thread, later a
// cosmetic world) creates its own: an own JPH::PhysicsSystem whose static bodies (one per tile and layer, created
// in tile order, then OptimizeBroadPhase) reference the shared, immutable StaticWorldShapes. Bodies cost a few
// hundred bytes each; the triangles exist once per process.
//
// Threads: every method runs on the owner's thread only (each world has its own temp allocator and query
// scratch). Two worlds over the same shapes may be queried at the same time from different threads. Plan §2.3's
// server job pool (bot perception rays between steps, B2+) needs queries from several threads on one world:
// Jolt's narrow-phase queries are thread-safe on a system that is not being modified, so B2 adds overloads that
// take a caller-owned scratch (temp allocator + collectors) per worker; M1 needs none.
// Determinism: two worlds built from the same shapes with the same options answer every query identically (same
// bodies, same creation order, same broadphase build), which is what makes the server's and the client's motor
// steps equal (docs §6.11). M1 never steps these systems (no dynamic body); destruction (M6) adds per-world
// mutable pieces without touching the shared shapes.

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/static_world.h"

#include <cstdint>
#include <memory>

namespace bf3::sim {

struct CollisionWorldOptions {
	uint32_t max_dynamic_bodies = 1024; // headroom for later milestones (vehicles, debris); M1 adds none
	uint32_t temp_allocator_bytes = 4u << 20;
};

// TC (docs/design/terrain-collision.md §5.3): a hit on a terrain field or its patch has tile = kNoStaticTile,
// terrain = its index into StaticWorldShapes::terrain, layer Collision and tag static_terrain_tag(); a tile hit has
// terrain = kNoStaticTerrain. Check tile < shapes->tile_count() before StaticWorldShapes::tile / tag.
inline constexpr uint32_t kNoStaticTile = 0xFFFF'FFFFu;
inline constexpr uint32_t kNoStaticTerrain = 0xFFFF'FFFFu;

struct StaticHit {
	Vec3 position{}; // world space
	Vec3 normal{}; // the triangle's face normal, towards the query
	float fraction = 1.0f; // along the ray or sweep (0..1)
	StaticLayer layer = StaticLayer::Collision;
	uint32_t tile = 0; // index into StaticWorldShapes::tile (kNoStaticTile on terrain, TC)
	SurfaceTag tag;
	uint32_t terrain = kNoStaticTerrain; // TC: index into StaticWorldShapes::terrain on a terrain hit
};

class CollisionWorld {
public:
	static core::Result<std::unique_ptr<CollisionWorld>> create(std::shared_ptr<const StaticWorldShapes> p_shapes,
			const CollisionWorldOptions &p_options = {});
	~CollisionWorld();
	CollisionWorld(const CollisionWorld &) = delete;
	CollisionWorld &operator=(const CollisionWorld &) = delete;

	const std::shared_ptr<const StaticWorldShapes> &shapes() const;

	// Closest hit of the segment p_origin -> p_origin + p_delta against the layers in p_mask (query_mask::).
	bool ray(const Vec3 &p_origin, const Vec3 &p_delta, uint32_t p_mask, StaticHit *r_hit = nullptr) const;
	// First contact of an upright capsule (feet at p_feet, total height p_height incl. both caps, radius
	// p_radius) swept by p_delta. A start already in contact reports fraction 0.
	bool sweep_capsule(const Vec3 &p_feet, float p_radius, float p_height, const Vec3 &p_delta, uint32_t p_mask, StaticHit *r_hit = nullptr) const;
	// Whether the upright capsule at p_feet overlaps static geometry deeper than p_tolerance (metres).
	bool overlap_capsule(const Vec3 &p_feet, float p_radius, float p_height, uint32_t p_mask, float p_tolerance = 0.0f) const;
	// Whether a box (half extents p_half, rotated by p_yaw about +Y, centred at p_center) overlaps static geometry.
	bool overlap_box(const Vec3 &p_center, const Vec3 &p_half, float p_yaw, uint32_t p_mask, float p_tolerance = 0.0f) const;
	// Highest surface below p_from within p_max_drop: the first hit of a ray straight down (r_hit may be null); false
	// when none. It does not judge the slope: the caller reads r_hit->normal (the motor's MaxAscendAngle decides).
	bool ground_below(const Vec3 &p_from, float p_max_drop, uint32_t p_mask, StaticHit *r_hit) const;

	// S1 (docs/design/s1.md §6.3): narrow-phase queries run against this world since create(), the motor's
	// CharacterVirtual casts included. Counted in the broadphase layer filters of collision_world_internal.h: with
	// one broadphase layer, Jolt's BroadPhaseQuadTree calls ShouldCollide(BroadPhaseLayer) exactly once per ray, cast
	// or collide query [verified: BroadPhaseQuadTree.cpp, every query loops the layers once]. Owner thread.
	uint64_t query_count() const;

	struct Impl; // bf3_sim internal (src/collision_world_internal.h): the PhysicsSystem, filters, temp allocator
	Impl &impl() const { return *impl_; }

private:
	explicit CollisionWorld(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::sim
