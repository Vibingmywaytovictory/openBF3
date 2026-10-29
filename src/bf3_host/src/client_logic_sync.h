#pragma once

#include "bf3/host/client_session.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/logic/logic_runtime.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace bf3::host {

inline constexpr size_t kClientLogicMaxQueuedEvents = 4096;
inline constexpr size_t kClientLogicMaxHeldDeltas = 1024;

class ClientLogicSync {
public:
	void datagram(net::MessageType p_type, std::span<const uint8_t> p_bytes);
	void take(ClientLogicFeed &r_feed);
	void stats(ClientStats &r_stats) const;

private:
	void events(const net::LogicEvents &p_message);
	void full_sync(net::FullSync p_message);
	void delta(const net::LogicDelta &p_message);
	void apply(const net::LogicDelta &p_message);

	mutable std::mutex mutex_;
	bool have_ghost_ = false;
	sim::logic::LogicGhost ghost_;
	sim::Tick sync_tick_ = 0;
	uint64_t revision_ = 0;
	std::vector<net::LogicDelta> held_;
	std::vector<sim::logic::LogicNetworkedEvent> queued_;
	bool any_event_ = false;
	sim::Tick first_event_tick_ = 0;
	uint64_t full_syncs_ = 0;
	uint64_t deltas_ = 0;
	uint64_t events_ = 0;
	uint64_t stale_ = 0;
	uint64_t early_events_ = 0;
	uint64_t dropped_events_ = 0;
	uint64_t bad_ = 0;
};

}
