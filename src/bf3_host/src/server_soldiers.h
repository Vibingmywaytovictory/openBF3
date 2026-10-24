#pragma once

#include "bf3/sim/command.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/soldier_state.h"
#include "server_slots.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::host {

struct Soldier {
	bool spawned = false;
	sim::SoldierState state;
	sim::Command last;
	uint16_t own_events = 0;
};

class Soldiers {
public:
	void resize(size_t p_count) { soldiers_.resize(p_count); }
	size_t size() const { return soldiers_.size(); }
	const Soldier &operator[](size_t p_slot) const { return soldiers_[p_slot]; }
	bool alive(size_t p_slot) const { return soldiers_[p_slot].spawned && soldier_alive(soldiers_[p_slot].state); }

	void enter(size_t p_slot, const sim::SoldierState &p_state) {
		Soldier &s = soldiers_[p_slot];
		s.state = p_state;
		s.spawned = true;
		s.last = sim::hold_command(s.state);
		s.own_events = uint16_t(s.own_events | s.state.events);
	}

	void replace(size_t p_slot, const sim::SoldierState &p_state) {
		Soldier &s = soldiers_[p_slot];
		s.state = p_state;
		s.spawned = true;
		s.last = sim::hold_command(s.state);
	}

	void enter_probe(size_t p_slot, const sim::SoldierState &p_state) {
		Soldier &s = soldiers_[p_slot];
		s.spawned = true;
		s.state = p_state;
	}

	void teleport(size_t p_slot, const sim::SoldierState &p_state) { soldiers_[p_slot].state = p_state; }
	void leave(size_t p_slot) { soldiers_[p_slot].spawned = false; }
	void clear(size_t p_slot) { soldiers_[p_slot] = Soldier(); }

	void command(size_t p_slot, const sim::Command &p_command) { soldiers_[p_slot].last = p_command; }
	void step(size_t p_slot, const sim::SoldierMotor &p_motor) { p_motor.step(soldiers_[p_slot].state, soldiers_[p_slot].last); }

	void take_rules(std::span<const sim::SoldierState> p_states, const SlotTable &p_slots) {
		for (size_t i = 0; i < soldiers_.size(); ++i) {
			if (p_slots[i].used && soldiers_[i].spawned && !p_slots[i].dummy) {
				soldiers_[i].state = p_states[i];
			}
		}
	}

	void collect_events(size_t p_slot) {
		Soldier &s = soldiers_[p_slot];
		s.own_events = uint16_t(s.own_events | s.state.events);
	}

	uint16_t take_events(size_t p_slot) {
		const uint16_t events = soldiers_[p_slot].own_events;
		soldiers_[p_slot].own_events = 0;
		return events;
	}

private:
	std::vector<Soldier> soldiers_;
};

}
