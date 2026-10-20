#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/static_world.h"

#include <cstdint>
#include <memory>

namespace bf3::sim {

struct CollisionWorldOptions {
	uint32_t max_dynamic_bodies = 1024;
	uint32_t temp_allocator_bytes = 4u << 20;
};

inline constexpr uint32_t kNoStaticTile = 0xFFFF'FFFFu;
inline constexpr uint32_t kNoStaticTerrain = 0xFFFF'FFFFu;

struct StaticHit {
	Vec3 position{};
	Vec3 normal{};
	float fraction = 1.0f;
	StaticLayer layer = StaticLayer::Collision;
	uint32_t tile = 0;
	SurfaceTag tag;
	uint32_t terrain = kNoStaticTerrain;
};

class CollisionWorld {
public:
	static core::Result<std::unique_ptr<CollisionWorld>> create(std::shared_ptr<const StaticWorldShapes> p_shapes,
			const CollisionWorldOptions &p_options = {});
	~CollisionWorld();
	CollisionWorld(const CollisionWorld &) = delete;
	CollisionWorld &operator=(const CollisionWorld &) = delete;

	const std::shared_ptr<const StaticWorldShapes> &shapes() const;

	bool ray(const Vec3 &p_origin, const Vec3 &p_delta, uint32_t p_mask, StaticHit *r_hit = nullptr) const;

	bool ray_any(const Vec3 &p_origin, const Vec3 &p_delta, uint32_t p_mask) const;

	bool sweep_capsule(const Vec3 &p_feet, float p_radius, float p_height, const Vec3 &p_delta, uint32_t p_mask, StaticHit *r_hit = nullptr) const;

	bool overlap_capsule(const Vec3 &p_feet, float p_radius, float p_height, uint32_t p_mask, float p_tolerance = 0.0f) const;

	bool overlap_box(const Vec3 &p_center, const Vec3 &p_half, float p_yaw, uint32_t p_mask, float p_tolerance = 0.0f) const;

	bool ground_below(const Vec3 &p_from, float p_max_drop, uint32_t p_mask, StaticHit *r_hit) const;

	uint64_t query_count() const;

	struct Impl;
	Impl &impl() const { return *impl_; }

private:
	explicit CollisionWorld(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

}
