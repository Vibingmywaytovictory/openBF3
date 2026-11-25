#pragma once

#include "../vehicle_world_internal.h"

#include <Jolt/Physics/Constraints/SpringSettings.h>

namespace bf3::sim {

void land_drive(VehicleWorld::Impl &r_world, VehicleSlot p_slot, const VehicleCommand &p_command);

void land_motion_damping(VehicleWorld::Impl &r_world, VehicleSlot p_slot);

void land_engine_update(VehicleWorld::Impl &r_world, VehicleSlot p_slot);

VehicleDriverState land_driver_at_rest(const VehicleModel &p_model);

JPH::SpringSettings land_spring(
		const VehicleModel &p_model, const VehicleWheelModel &p_wheel, float p_speed, float p_compression);
}
