// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIUtils/VehicleOccupancy.h @c47317b
#pragma once

#include <cstdint>

namespace bf3::ai {

class InformationManager;

bool vehicle_occupied(const InformationManager &p_information, uint32_t p_handle, int p_side);

}
