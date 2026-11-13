#pragma once

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/math.h"
#include "bf3/sim/vehicles/vehicle_armament.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::present {

std::vector<core::Transform> turret_offsets(
		const sim::vehicles::VehicleArmament &p_armament, std::span<const float> p_angles);

int part_turret(std::span<const core::gameplay::VehiclePartTurretDef> p_parts, std::span<const int> p_component_meshes,
		uint8_t p_mesh, uint16_t p_part);

}
