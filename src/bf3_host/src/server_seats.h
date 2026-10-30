#pragma once

#include "bf3/host/game_server.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/sim_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::host {

class SeatLoads {
public:
	void configure(size_t p_slots, const LoadDeadlines &p_deadlines, uint32_t p_round_id);
	void begin(size_t p_slot, sim::Tick p_now);
	bool progress(size_t p_slot, const net::Loading &p_message, sim::Tick p_now);
	void done(size_t p_slot);
	void reset(size_t p_slot);

	const SeatLoad &operator[](size_t p_slot) const { return seats_[p_slot]; }
	uint32_t round_id() const { return round_id_; }

	struct Due {
		size_t slot = 0;
		net::KickReason reason = net::KickReason::LoadTimeout;
		std::string detail;
	};
	std::vector<Due> due(sim::Tick p_now) const;

	uint64_t stale_reports() const { return stale_; }

private:
	std::vector<SeatLoad> seats_;
	sim::Tick stall_ticks_ = 0;
	sim::Tick warm_ticks_ = 0;
	sim::Tick cold_ticks_ = 0;
	uint32_t round_id_ = 0;
	uint64_t stale_ = 0;
};

}
