#pragma once

#include "bf3/sim/weapons/gadgets.h"

#include <cstdint>
#include <span>

namespace bf3::host {

struct GadgetStats {
	uint64_t deploys = 0;
	uint64_t refused = 0;
	uint64_t heal_ticks = 0;
	double healed = 0;
	uint64_t ammo_ticks = 0;
	uint64_t rounds_refilled = 0;
	uint64_t rounds_replenished = 0;
	uint64_t forgotten = 0;
	uint64_t remote_detonations = 0;
	uint64_t pack_detonations = 0;
	sim::weapons::GadgetFieldStats field;
};

uint64_t gadget_defs_hash(
		std::span<const sim::weapons::GadgetDef> p_gadgets, std::span<const sim::weapons::AmmoPickupDef> p_pickups);

}
