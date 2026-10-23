#include "server_commands.h"

#include <algorithm>

namespace bf3::host {

void CommandBuffers::configure(size_t p_slots, uint32_t p_jitter_buffer_ticks) {
	ahead_ = std::clamp<uint32_t>(p_jitter_buffer_ticks, 1u, 1024u);
	streams_.resize(p_slots);
}

void CommandBuffers::reset(size_t p_slot) {
	Stream &s = streams_[p_slot];
	s = Stream();
	s.buffer.assign(ahead_ + 1u, Buffered{});
}

std::span<const sim::Command> CommandBuffers::receive(size_t p_slot, const net::CommandPacket &p_packet, uint64_t p_now_ns, sim::Tick p_tick) {
	Stream &s = streams_[p_slot];
	buffered_.clear();
	const uint32_t size = uint32_t(s.buffer.size());
	for (size_t k = p_packet.count; k-- > 0;) {
		sim::Command c = p_packet.commands[k];
		c.flags = 0;
		if (!s.any_command || c.sequence > s.highest_sequence) {
			s.any_command = true;
			s.highest_sequence = c.sequence;
			s.highest_sequence_tick = c.tick;
			s.highest_sequence_ns = p_now_ns;
		}
		if (c.tick <= p_tick) {
			if (c.sequence > s.last_applied_sequence && c.sequence > s.highest_late_sequence) {
				s.highest_late_sequence = c.sequence;
				++s.late;
			}
			continue;
		}
		if (c.tick - p_tick > ahead_) {
			continue;
		}
		Buffered &b = s.buffer[c.tick % size];
		if (!b.valid || b.command.tick != c.tick) {
			b.valid = true;
			b.command = c;
			buffered_.push_back(c);
		} else if (c.sequence > b.command.sequence) {
			b.command = c;
			++s.replaced;
			buffered_.push_back(c);
		}
	}
	return buffered_;
}

std::optional<sim::Command> CommandBuffers::take(size_t p_slot, sim::Tick p_tick) {
	Stream &s = streams_[p_slot];
	Buffered &b = s.buffer[p_tick % s.buffer.size()];
	std::optional<sim::Command> taken;
	if (b.valid && b.command.tick == p_tick) {
		taken = b.command;
	}
	if (b.valid && b.command.tick <= p_tick) {
		b.valid = false;
	}
	return taken;
}

sim::Command CommandBuffers::apply(size_t p_slot, const std::optional<sim::Command> &p_taken, const sim::Command &p_last, sim::Tick p_tick) {
	Stream &s = streams_[p_slot];
	if (p_taken) {
		s.applied_sequence = p_taken->sequence;
		s.applied_repeated = false;
		s.last_applied_sequence = std::max(s.last_applied_sequence, p_taken->sequence);
		++s.applied;
		return *p_taken;
	}
	const sim::Command repeated = sim::repeated_command(p_last, p_tick);
	s.applied_sequence = repeated.sequence;
	s.applied_repeated = true;
	++s.repeated;
	return repeated;
}

void CommandBuffers::count_local(size_t p_slot) {
	++streams_[p_slot].applied;
}

void CommandBuffers::stamp(size_t p_slot, sim::Tick p_tick, uint64_t p_now_ns, net::Snapshot &r_snapshot) const {
	const Stream &s = streams_[p_slot];
	r_snapshot.applied_sequence = s.applied_sequence;
	r_snapshot.applied_repeated = s.applied_repeated;
	const int64_t buffer = s.any_command ? int64_t(s.highest_sequence_tick) - int64_t(p_tick) : 0;
	r_snapshot.command_buffer_ticks = int8_t(std::clamp<int64_t>(buffer, -127, 127));
	r_snapshot.sequence_echo = uint16_t(s.highest_sequence & 0xFFFFu);
	if (s.any_command) {
		const uint64_t hold = p_now_ns > s.highest_sequence_ns ? (p_now_ns - s.highest_sequence_ns) / 100000u : 0u;
		r_snapshot.echo_hold_100us = uint16_t(std::min<uint64_t>(hold, 0xFFFFu));
	}
}

void CommandBuffers::stats(size_t p_slot, sim::Tick p_tick, SlotStats &r_stats) const {
	const Stream &s = streams_[p_slot];
	r_stats.commands_applied = s.applied;
	r_stats.commands_repeated = s.repeated;
	r_stats.commands_late = s.late;
	r_stats.commands_replaced = s.replaced;
	r_stats.buffer_ticks = s.any_command ? int32_t(int64_t(s.highest_sequence_tick) - int64_t(p_tick)) : 0;
}

}
