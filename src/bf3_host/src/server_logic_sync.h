#pragma once

#include "bf3/net/protocol.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/sim_types.h"
#include "server_match.h"
#include "server_outbox.h"
#include "server_slots.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bf3::host {

struct ServerLogicSyncStats {
	uint64_t full_syncs = 0;
	uint64_t resyncs = 0;
	uint64_t deltas = 0;
	uint64_t event_messages = 0;
	uint64_t events_sent = 0;
	uint64_t events_refused = 0;
};

class ServerLogicSync {
public:
	ServerLogicSync(const SlotTable &p_slots, MatchControl &r_match, Outbox &r_outbox) :
			slots_(p_slots), match_(r_match), outbox_(r_outbox) {}

	void resize(size_t p_slots) { ready_.assign(p_slots, 0); }
	void reset(size_t p_slot) { ready_[p_slot] = 0; }
	void ready(size_t p_slot);
	void send(sim::Tick p_tick);

	const ServerLogicSyncStats &stats() const { return stats_; }

private:
	void broadcast(uint8_t p_channel, const std::vector<uint8_t> &p_bytes);

	const SlotTable &slots_;
	MatchControl &match_;
	Outbox &outbox_;
	std::vector<uint8_t> ready_;
	std::optional<sim::logic::LogicGhost> sent_;
	ServerLogicSyncStats stats_;
};

}
