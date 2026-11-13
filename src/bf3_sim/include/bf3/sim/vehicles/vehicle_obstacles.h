#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace bf3::sim {

struct ObstacleContact {
	uint32_t slot = 0;
	Vec3 position{};
	Vec3 normal{};
	Vec3 velocity{};
};

class VehicleObstacles {
public:
	VehicleObstacles();
	~VehicleObstacles();
	VehicleObstacles(const VehicleObstacles &) = delete;
	VehicleObstacles &operator=(const VehicleObstacles &) = delete;

	core::Status place(
			uint32_t p_slot, const std::shared_ptr<const VehicleModel> &p_model, const VehicleBodyState &p_body);
	void remove(uint32_t p_slot);
	void clear();
	size_t count() const;
	bool holds(uint32_t p_slot) const;
	bool overlaps_capsule(const Vec3 &p_feet, float p_radius, float p_height) const;

	struct Impl;
	Impl &impl() const { return *impl_; }

private:
	std::unique_ptr<Impl> impl_;
};

}
