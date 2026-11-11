#pragma once

#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "server_outbox.h"
#include "server_slots.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bf3::host {

class ConquestSync {
public:
	void send(sim::Tick p_tick, const TdmRules &p_rules, const SlotTable &p_slots, Outbox &r_outbox);
	uint64_t messages() const { return messages_; }

private:
	struct Sent {
		bool valid = false;
		uint32_t peer = 0;
		sim::Tick tick = 0;
		net::ConquestState state;
	};
	std::vector<Sent> sent_;
	uint64_t messages_ = 0;
};

}
