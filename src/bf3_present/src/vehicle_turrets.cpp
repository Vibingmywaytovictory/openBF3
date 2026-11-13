#include "bf3/present/vehicle_turrets.h"

namespace bf3::present {

int part_turret(std::span<const core::gameplay::VehiclePartTurretDef> p_parts, std::span<const int> p_component_meshes,
		uint8_t p_mesh, uint16_t p_part) {
	if (p_mesh > 0) {
		return size_t(p_mesh - 1) < p_component_meshes.size() ? p_component_meshes[size_t(p_mesh - 1)] : -1;
	}
	for (const core::gameplay::VehiclePartTurretDef &p : p_parts) {
		if (p.part == p_part) {
			return p.turret;
		}
	}
	return -1;
}

}
