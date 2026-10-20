#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/bots/bot_stats.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::sim {

inline constexpr uint32_t kMaxNavPathPolys = 512;
inline constexpr uint32_t kMaxNavCorners = 8;

struct NavQueryOptions {
	uint32_t max_nodes = 4096;
	uint16_t include_flags = nav_flags::kAll;
	uint16_t exclude_flags = 0;
};

enum class NavPathStatus : uint8_t { Idle = 0, InProgress = 1, Done = 2, Partial = 3, Failed = 4 };

class NavQuery {
public:
	static core::Result<std::unique_ptr<NavQuery>> create(std::shared_ptr<const NavWorld> p_world, const NavQueryOptions &p_options = {});
	~NavQuery();
	NavQuery(const NavQuery &) = delete;
	NavQuery &operator=(const NavQuery &) = delete;

	const NavWorld &world() const;
	const NavQueryStats &stats() const;

	bool nearest(const Vec3 &p_point, const Vec3 &p_half_extents, NavPolyRef *r_poly, Vec3 *r_point);

	bool random_point(uint64_t &r_rng, const NavComponents &p_components, uint32_t p_component, NavPolyRef *r_poly, Vec3 *r_point);

	NavPathStatus begin_path(NavPolyRef p_start, const Vec3 &p_start_point, NavPolyRef p_goal, const Vec3 &p_goal_point);
	NavPathStatus continue_path(uint32_t p_max_iterations, uint32_t *r_iterations);
	NavPathStatus finish_path(std::vector<NavPolyRef> &r_polys);
	void cancel_path();

	struct Impl;
	Impl &impl() { return *impl_; }

private:
	explicit NavQuery(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

struct NavCorner {
	Vec3 position{};
	uint16_t flags = 0;
	uint8_t area = 0;
	bool end = false;
	NavPolyRef poly = 0;
};

class NavCorridor {
public:
	NavCorridor();
	~NavCorridor();
	NavCorridor(NavCorridor &&) noexcept;
	NavCorridor &operator=(NavCorridor &&) noexcept;

	void reset(std::span<const NavPolyRef> p_polys, const Vec3 &p_position, const Vec3 &p_goal);
	void clear();
	bool empty() const;

	bool move_position(NavQuery &p_query, const Vec3 &p_position);

	uint32_t corners(NavQuery &p_query, NavCorner *r_corners, uint32_t p_max);

	bool valid(NavQuery &p_query, uint32_t p_ahead) const;

	bool move_over_drop(NavQuery &p_query, const NavCorner &p_corner, Vec3 *r_end);
	Vec3 position() const;
	Vec3 goal() const;
	NavPolyRef first_poly() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
