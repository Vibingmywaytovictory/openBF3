#pragma once

#include "bf3/host/game_server.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf3::host {

class CommandBuffers {
public:
	void configure(size_t p_slots, uint32_t p_jitter_buffer_ticks);
	void reset(size_t p_slot);

	std::span<const sim::Command> receive(size_t p_slot, const net::CommandPacket &p_packet, uint64_t p_now_ns, sim::Tick p_tick);

	std::optional<sim::Command> take(size_t p_slot, sim::Tick p_tick);
	sim::Command apply(size_t p_slot, const std::optional<sim::Command> &p_taken, const sim::Command &p_last, sim::Tick p_tick);
	void count_local(size_t p_slot);

	void stamp(size_t p_slot, sim::Tick p_tick, uint64_t p_now_ns, net::Snapshot &r_snapshot) const;
	void stats(size_t p_slot, sim::Tick p_tick, SlotStats &r_stats) const;

private:
	struct Buffered {
		bool valid = false;
		sim::Command command;
	};

	struct Stream {
		std::vector<Buffered> buffer;
		uint32_t applied_sequence = 0;
		bool applied_repeated = true;
		bool any_command = false;
		uint32_t highest_sequence = 0;
		sim::Tick highest_sequence_tick = 0;
		uint64_t highest_sequence_ns = 0;
		uint32_t last_applied_sequence = 0;
		uint32_t highest_late_sequence = 0;
		uint64_t applied = 0;
		uint64_t repeated = 0;
		uint64_t late = 0;
		uint64_t replaced = 0;
	};

	uint32_t ahead_ = 1;
	std::vector<Stream> streams_;
	std::vector<sim::Command> buffered_;
};

}
