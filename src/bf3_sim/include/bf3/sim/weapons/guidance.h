#pragma once

#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/locking.h"
#include "bf3/sim/weapons/sim_time.h"

#include <array>
#include <cstdint>
#include <optional>

namespace bf3::sim::weapons {

struct ProjectileSimDef;

using DVec3 = std::array<double, 3>;

enum class MissileState : uint8_t {
	PreIgnition = 0,
	Powered = 1,
	StaticPoint = 2,
	TargetOffset = 3,
	Homing = 4,
	BurntOut = 5,
	Proximity = 6,
};

inline constexpr double kSteerThreshold = 1e-6;
inline constexpr double kVerticalLaunchFloor = 1e-6;
inline constexpr DVec3 kVerticalStaticPoint{ 0.0, 100.0, 0.0 };

struct GuidanceTarget {
	Vec3 position{};
	Vec3 aim{};
};

class GuidanceTargets {
public:
	virtual ~GuidanceTargets() = default;
	virtual std::optional<GuidanceTarget> guidance_target(uint32_t p_id) const = 0;
};

struct MissileGuide {
	MissileState state = MissileState::PreIgnition;
	uint32_t target = kNoLockTarget;
	bool has_static = false;
	DVec3 static_point{};
	SimTime detonate_at = 0;
};

struct MissileStep {
	MissileGuide guide;
	DVec3 velocity{};
	bool engine = false;
	bool detonate = false;
};

DVec3 missile_static_point(const DVec3 &p_origin, const DVec3 &p_direction, const std::array<float, 2> &p_static);

DVec3 steer_toward(const DVec3 &p_velocity, const DVec3 &p_position, const DVec3 &p_target, double p_max_turn_rad);

MissileStep step_guided_missile(const ProjectileSimDef &p_def, const MissileGuide &p_guide, const DVec3 &p_position,
		const DVec3 &p_velocity, double p_flight, double p_dt, SimTime p_now,
		const std::optional<GuidanceTarget> &p_target, uint64_t p_seed);

}
