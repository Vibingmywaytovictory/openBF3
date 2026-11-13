#pragma once

#include "bf3/core/gameplay/vehicle_def.h"

#include <cstdint>
#include <span>

namespace bf3::present {

int part_turret(std::span<const core::gameplay::VehiclePartTurretDef> p_parts, std::span<const int> p_component_meshes,
		uint8_t p_mesh, uint16_t p_part);

}
