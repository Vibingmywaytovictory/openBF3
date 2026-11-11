#include "server_conquest.h"

#include "bf3/host/conquest_wire.h"

namespace bf3::host {

void ConquestSync::send(sim::Tick p_tick, const TdmRules &p_rules, const SlotTable &p_slots, Outbox &r_outbox) {
	const sim::logic::ConquestNatives *conquest = p_rules.natives().conquest();
	if (conquest == nullptr) {
		return;
	}
	sent_.resize(p_slots.size());
	const net::ConquestState common = conquest_wire_state(*conquest, p_tick);
	for (size_t i = 0; i < p_slots.size(); ++i) {
		const Seat &seat = p_slots[i];
		Sent &sent = sent_[i];
		if (!seat.used || !seat.remote) {
			sent.valid = false;
			continue;
		}
		net::ConquestState now = common;
		now.return_by_tick = conquest_return_by(*conquest, p_tick, uint8_t(i));
		if (sent.valid && sent.peer == seat.peer && !conquest_resend(sent.state, now, sent.tick, p_tick)) {
			continue;
		}
		r_outbox.send(seat.peer, net::kChannelReliable, net::encode_conquest_state(now));
		sent.valid = true;
		sent.peer = seat.peer;
		sent.tick = p_tick;
		sent.state = now;
		++messages_;
	}
}

}
