#pragma once

#include "bf3/net/weapons_wire.h"
#include "bit_archive.h"

namespace bf3::net {

struct WeaponsWireFaults {
	const char *slots = nullptr;
	const char *field = nullptr;
};

void serialize(BitWriteArchive &r_archive, const WeaponsWire &p_wire, const WeaponsWireFaults &p_faults);
void serialize(BitReadArchive &r_archive, WeaponsWire &r_wire, const WeaponsWireFaults &p_faults);

}
