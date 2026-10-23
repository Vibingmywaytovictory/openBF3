#pragma once

#include "bf3/host/game_server.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::host {

inline bool soldier_alive(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead;
}

struct ServerSlot {
	bool used = false;
	bool remote = false;
	bool dummy = false;
	bool bot = false;
	uint32_t peer = 0;
	int team = 1;
	bool hello = false;
	bool spawned = false;
	bool want_spawn = false;
	sim::Tick respawn_at = 0;
	uint32_t spawn_index = 0;
	sim::SoldierState state;
	sim::Command last;
	uint16_t own_events = 0;
	std::unique_ptr<CommandSource> source;
	sim::Command source_command;

	bool tracing = false;
	uint32_t trace_id = 0;
	uint32_t trace_ticks = 0;
	std::vector<sim::SoldierState> trace;

	size_t sweep_entry = 0;
};

class SlotTable {
public:
	void resize(size_t p_count) { slots_.resize(p_count); }
	size_t size() const { return slots_.size(); }
	ServerSlot &operator[](size_t p_slot) { return slots_[p_slot]; }
	const ServerSlot &operator[](size_t p_slot) const { return slots_[p_slot]; }
	std::vector<ServerSlot>::iterator begin() { return slots_.begin(); }
	std::vector<ServerSlot>::iterator end() { return slots_.end(); }
	std::vector<ServerSlot>::const_iterator begin() const { return slots_.begin(); }
	std::vector<ServerSlot>::const_iterator end() const { return slots_.end(); }

	size_t index_of(const ServerSlot &p_slot) const { return size_t(&p_slot - slots_.data()); }
	void clear(size_t p_slot) { slots_[p_slot] = ServerSlot(); }

	int slot_of_peer(uint32_t p_peer) const {
		for (size_t i = 0; i < slots_.size(); ++i) {
			if (slots_[i].used && slots_[i].remote && slots_[i].peer == p_peer) {
				return int(i);
			}
		}
		return -1;
	}

	int free_slot() const {
		for (size_t i = 0; i < slots_.size(); ++i) {
			if (!slots_[i].used) {
				return int(i);
			}
		}
		return -1;
	}

	std::vector<sim::Vec3> live_positions(size_t p_except) const {
		std::vector<sim::Vec3> out;
		for (size_t i = 0; i < slots_.size(); ++i) {
			const ServerSlot &s = slots_[i];
			if (i != p_except && s.used && !s.dummy && s.spawned && soldier_alive(s.state)) {
				out.push_back(s.state.position);
			}
		}
		return out;
	}

private:
	std::vector<ServerSlot> slots_;
};

}
