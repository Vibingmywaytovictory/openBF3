#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/logic/conquest_natives.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>

namespace bf3::host {

inline constexpr sim::Tick kConquestProgressIntervalTicks = 15;

net::ConquestState conquest_wire_state(const sim::logic::ConquestNatives &p_conquest, sim::Tick p_tick);
sim::Tick conquest_return_by(const sim::logic::ConquestNatives &p_conquest, sim::Tick p_tick, uint8_t p_slot);

bool conquest_resend(
		const net::ConquestState &p_sent, const net::ConquestState &p_now, sim::Tick p_sent_tick, sim::Tick p_tick);

}
