#pragma once

#include "bf3/host/game_server.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::host {

inline bool soldier_alive(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead;
}

struct Seat {
	bool used = false;
	bool remote = false;
	bool dummy = false;
	bool bot = false;
	uint32_t peer = 0;
	int team = 1;
	std::string name;
};

class SlotTable {
public:
	void resize(size_t p_count) {
		seats_.resize(p_count);
		sources_.resize(p_count);
	}
	size_t size() const { return seats_.size(); }
	const Seat &operator[](size_t p_slot) const { return seats_[p_slot]; }

	void admit_client(size_t p_slot, uint32_t p_peer, int p_team) {
		Seat &s = seats_[p_slot];
		s.used = true;
		s.remote = true;
		s.peer = p_peer;
		s.team = p_team;
	}

	void admit_local(size_t p_slot, std::unique_ptr<CommandSource> p_source, int p_team, bool p_bot) {
		Seat &s = seats_[p_slot];
		s.used = true;
		s.team = p_team;
		s.bot = p_bot;
		sources_[p_slot] = std::move(p_source);
	}

	void admit_dummy(size_t p_slot) {
		Seat &s = seats_[p_slot];
		s.used = true;
		s.dummy = true;
		s.team = 0;
	}

	void assign_team(size_t p_slot, int p_team) { seats_[p_slot].team = p_team; }
	void name(size_t p_slot, std::string_view p_name) { seats_[p_slot].name = std::string(p_name); }

	void clear(size_t p_slot) {
		seats_[p_slot] = Seat();
		sources_[p_slot].reset();
	}

	sim::Command source_command(size_t p_slot, sim::Tick p_tick, const sim::SoldierState &p_state) {
		return sources_[p_slot] != nullptr ? sources_[p_slot]->command(p_tick, p_state) : sim::Command();
	}

	int slot_of_peer(uint32_t p_peer) const {
		for (size_t i = 0; i < seats_.size(); ++i) {
			if (seats_[i].used && seats_[i].remote && seats_[i].peer == p_peer) {
				return int(i);
			}
		}
		return -1;
	}

	int free_slot() const {
		for (size_t i = 0; i < seats_.size(); ++i) {
			if (!seats_[i].used) {
				return int(i);
			}
		}
		return -1;
	}

private:
	std::vector<Seat> seats_;
	std::vector<std::unique_ptr<CommandSource>> sources_;
};

}
