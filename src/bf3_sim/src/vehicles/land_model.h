#pragma once

#include "bf3/core/gameplay/vehicle_drive.h"
#include "bf3/core/result.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"

namespace bf3::sim {

core::Status build_land_drive(const core::gameplay::VehicleDriveDef &p_def, LandDriveModel &r_land);

bool land_driver_state_valid(const VehicleModel &p_model, const VehicleDriverState &p_driver);

}
