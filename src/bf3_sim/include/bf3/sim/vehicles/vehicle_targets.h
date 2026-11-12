#pragma once

#include "bf3/core/math.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicles/vehicle_damage.h"
#include "bf3/sim/weapons/hit_target.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::sim::vehicles {

inline constexpr uint32_t kVehicleTargetIdBase = 0x10000u;
inline constexpr size_t kMaxVehicleTargetCrew = 8;

struct VehicleTarget {
	weapons::HitTarget target;
	std::array<uint32_t, kMaxVehicleTargetCrew> crew{};
	uint8_t crew_count = 0;
};

HullBox chassis_box(const VehicleModel &p_model);

weapons::HitTarget hull_target(const HullBox &p_box, const core::Transform &p_hull, uint32_t p_id, uint8_t p_property);

Vec3 to_hull(const core::Transform &p_hull, const Vec3 &p_world);
Vec3 to_hull_direction(const core::Transform &p_hull, const Vec3 &p_world);

class JoinedTargets final : public weapons::HitTargetProvider {
public:
	JoinedTargets(const weapons::HitTargetProvider &p_first, std::span<const VehicleTarget> p_vehicles) :
			first_(p_first), vehicles_(p_vehicles) {}
	std::span<const weapons::HitTarget> targets_for(const weapons::TargetQuery &p_query) const override;

private:
	const weapons::HitTargetProvider &first_;
	std::span<const VehicleTarget> vehicles_;
	mutable std::vector<weapons::HitTarget> joined_;
};

}
