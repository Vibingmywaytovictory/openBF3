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

struct VehicleLookLimits {
	float centre = 0.0f;
	float left = 0.0f, right = 0.0f, up = 0.0f, down = 0.0f;
};

VehicleLookLimits camera_look_limits(const core::gameplay::VehicleCameraDef &p_camera);

int part_turret(std::span<const core::gameplay::VehiclePartTurretDef> p_parts, std::span<const int> p_component_meshes,
		uint8_t p_mesh, uint16_t p_part);

}
