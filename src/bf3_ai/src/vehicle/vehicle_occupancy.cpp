#include "bf3/ai/vehicle/vehicle_occupancy.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"

namespace bf3::ai {

namespace {

Information *lookup(const InformationManager &p_information, uint32_t p_handle) {
	return (p_handle & kHandleIndexMask) != 0 ? p_information.information(p_handle) : nullptr;
}

}

bool vehicle_occupied(const InformationManager &p_information, uint32_t p_handle, int p_side) {
	const Information *primary = lookup(p_information, p_handle);
	if (primary == nullptr) {
		return false;
	}
	if (!primary->is_primary()) {
		primary = lookup(p_information, primary->primary_object(p_side));
		if (primary == nullptr) {
			return false;
		}
	}
	const IPIUnit *unit = primary->unit();
	if (unit == nullptr) {
		return false;
	}
	if (unit->is_occupied()) {
		return true;
	}
	int iterator = 0;
	const Information *secondary = lookup(p_information, primary->first_secondary_object(iterator, p_side));
	while (secondary != nullptr) {
		unit = secondary->unit();
		if (unit != nullptr && unit->is_occupied()) {
			return true;
		}
		secondary = lookup(p_information, primary->secondary_object(iterator, p_side));
	}
	return false;
}

}
