#pragma once
// Queries on a NavWorld (spike S1; plan §3.6; docs/design/s1.md §5.6, §7). One NavQuery per thread (Detour's
// dtNavMeshQuery keeps its node pool and open list per instance, DetourNavMeshQuery.h); any number of NavQuerys read
// one NavWorld at once. Godot-free; no Detour type in this header.
//
// Bounds (hostile or degenerate meshes, docs §9): every search runs on the instance's node pool (max_nodes), a path
// holds at most kMaxNavPathPolys polys and a corner list kMaxNavCorners corners; a sliced search advances at most the
// iterations it is given, so a caller (the bots' path service) bounds the work per tick.

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
	uint32_t max_nodes = 4096; // dtNavMeshQuery::init node pool
	uint16_t include_flags = nav_flags::kAll;
	uint16_t exclude_flags = 0;
};

// NavQueryStats (the cumulative counters of one NavQuery) is in bots/bot_stats.h, so GameServerStats can carry it
// without this header.

enum class NavPathStatus : uint8_t { Idle = 0, InProgress = 1, Done = 2, Partial = 3, Failed = 4 };

class NavQuery {
public:
	static core::Result<std::unique_ptr<NavQuery>> create(std::shared_ptr<const NavWorld> p_world, const NavQueryOptions &p_options = {});
	~NavQuery();
	NavQuery(const NavQuery &) = delete;
	NavQuery &operator=(const NavQuery &) = delete;

	const NavWorld &world() const;
	const NavQueryStats &stats() const;

	// The nearest poly in a box of p_half_extents around p_point and its closest point; false when none.
	bool nearest(const Vec3 &p_point, const Vec3 &p_half_extents, NavPolyRef *r_poly, Vec3 *r_point);
	// A random point on the mesh, uniform by area, inside component p_component (UINT32_MAX = any); p_rng is a
	// splitmix64 state the call advances (deterministic per bot). false after 64 draws outside the component.
	bool random_point(uint64_t &r_rng, const NavComponents &p_components, uint32_t p_component, NavPolyRef *r_poly, Vec3 *r_point);

	// One sliced search at a time per NavQuery (Detour's rule): begin, then continue with an iteration budget until
	// it is not InProgress, then take the polys (start first, at most kMaxNavPathPolys).
	NavPathStatus begin_path(NavPolyRef p_start, const Vec3 &p_start_point, NavPolyRef p_goal, const Vec3 &p_goal_point);
	NavPathStatus continue_path(uint32_t p_max_iterations, uint32_t *r_iterations);
	NavPathStatus finish_path(std::vector<NavPolyRef> &r_polys);
	void cancel_path();

	struct Impl; // src/nav_internal.h
	Impl &impl() { return *impl_; }

private:
	explicit NavQuery(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

// A walker's corridor along a found path (Detour's dtPathCorridor): the path's polys from the agent's current poly
// to the goal, kept valid while the agent moves (move_position follows the agent along the mesh surface), with the
// next straight-path corners for steering.
struct NavCorner {
	Vec3 position{};
	uint16_t flags = 0; // nav_flags of the poly the corner leaves into (kDrop: an off-mesh drop starts here)
	uint8_t area = 0; // kNavArea* of that poly
	bool end = false; // the goal
};

class NavCorridor {
public:
	NavCorridor();
	~NavCorridor();
	NavCorridor(NavCorridor &&) noexcept;
	NavCorridor &operator=(NavCorridor &&) noexcept;

	// Starts the corridor at p_position on p_polys[0] towards p_goal along p_polys.
	void reset(std::span<const NavPolyRef> p_polys, const Vec3 &p_position, const Vec3 &p_goal);
	void clear();
	bool empty() const;
	// Moves the corridor's start to the agent's new position along the mesh surface; false when the agent left the
	// corridor's polys (off the mesh, pushed into another component): the caller repaths.
	bool move_position(NavQuery &p_query, const Vec3 &p_position);
	// Up to p_max (<= kMaxNavCorners) straight-path corners from the current position; returns how many.
	uint32_t corners(NavQuery &p_query, NavCorner *r_corners, uint32_t p_max);
	// Whether the first p_ahead polys still exist in the mesh (a tile swap would invalidate them; S1 never swaps).
	bool valid(NavQuery &p_query, uint32_t p_ahead) const;
	Vec3 position() const;
	Vec3 goal() const;
	NavPolyRef first_poly() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::sim
