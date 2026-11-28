#pragma once

#include "bf3/sim/sim_types.h"

#include <optional>

namespace bf3::core::res {
struct Shape;
}

namespace bf3::sim::vehicles {

struct ChassisVolume {
	float volume = 0;
	Vec3 centroid{};
	Vec3 unit_inertia{};
};

// SOURCE: notes R8-G1-inertia rule 2 (density 1; box half extents and cylinder ends and radius grown by the convex
// radius; translate and transform children moved).
// INFERRED: list children are summed by volume about their common centroid, overlaps kept (R8-G1-inertia rule 2).
// Settle: a note that decodes the middleware's list combine.
std::optional<ChassisVolume> havok_chassis_volume(const core::res::Shape &p_shape);

// SOURCE: notes R8-G1-inertia rules 3 and 5 (the diagonal scaled to the mass, x RigidBodyData.InertiaModifier, then
// x VehicleConfigData.InertiaModifier).
Vec3 chassis_inertia(
		const ChassisVolume &p_volume, float p_mass, const Vec3 &p_body_modifier, const Vec3 &p_vehicle_modifier);

}
