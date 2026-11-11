#pragma once

#include "../vehicle_world_internal.h"

namespace bf3::sim {

void land_drive(VehicleWorld::Impl &r_world, VehicleSlot p_slot, const VehicleCommand &p_command);

void land_engine_update(VehicleWorld::Impl &r_world, VehicleSlot p_slot);

VehicleDriverState land_driver_at_rest(const VehicleModel &p_model);

}
