#pragma once

#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace bf3::sim {

class SoldierMotor;
class WeaponLane;

struct PredictionEntry {
	Command command;
	SoldierState state;
	bool sent = false;
};
static_assert(std::is_trivially_copyable_v<PredictionEntry>);

class PredictionRing {
public:
	explicit PredictionRing(uint32_t p_capacity = 128);
	uint32_t capacity() const;

	const PredictionEntry *find(Tick p_tick) const;

	const PredictionEntry *newest_at_or_below(Tick p_tick) const;

	void put(const PredictionEntry &p_entry);

	void discard_after(Tick p_tick);
	void clear();

	void attach_weapons(WeaponLane *p_lane);
	WeaponLane *weapons() const;

private:
	std::vector<PredictionEntry> slots_;
	std::vector<uint8_t> used_;
	WeaponLane *weapons_ = nullptr;
};

struct CatchUpStats {
	uint32_t steps = 0;
	uint32_t synthesized = 0;
};

CatchUpStats catch_up(const SoldierMotor &p_motor, const SoldierState &p_base, PredictionRing &r_ring, Tick p_until);

}

namespace bf3::sim {

struct WeaponPredictionEntry {
	Tick tick = 0;
	Tick spawn_tick = 0;
	weapons::SoldierWeaponsState state;
};
static_assert(std::is_trivially_copyable_v<WeaponPredictionEntry>);

class WeaponEmitSink {
public:
	virtual ~WeaponEmitSink() = default;
	virtual void emitted(const Command &p_command, const weapons::WeaponTickOutput &p_output, const WeaponPredictionEntry &p_after) = 0;
};

struct WeaponLaneStats {
	uint64_t steps = 0;
	uint64_t emitted_ticks = 0;
	uint64_t replayed_ticks = 0;
	uint64_t dead_ticks = 0;
	uint64_t gaps = 0;
};

class WeaponLane {
public:
	WeaponLane(uint32_t p_capacity, std::shared_ptr<const weapons::WeaponTable> p_table, const MotorParams &p_motor, uint64_t p_match_seed);

	uint32_t capacity() const;
	const WeaponPredictionEntry *find(Tick p_tick) const;
	const WeaponPredictionEntry *newest_at_or_below(Tick p_tick) const;
	void put(const WeaponPredictionEntry &p_entry);
	void clear();

	WeaponPredictionEntry step(const WeaponPredictionEntry &p_from, const Command &p_command, const SoldierState &p_before, const SoldierState &p_after);

	void set_sink(WeaponEmitSink *p_sink);
	Tick emitted_through() const;
	void set_emitted_through(Tick p_tick);
	const weapons::WeaponTable &table() const;
	std::shared_ptr<const weapons::WeaponTable> table_ptr() const;
	uint64_t match_seed() const;
	const WeaponLaneStats &stats() const;

private:
	std::shared_ptr<const weapons::WeaponTable> table_;
	MotorParams motor_;
	uint64_t match_seed_ = 0;
	std::vector<WeaponPredictionEntry> slots_;
	std::vector<uint8_t> used_;
	WeaponEmitSink *sink_ = nullptr;
	Tick emitted_through_ = 0;
	WeaponLaneStats stats_;
};

class WeaponReplay {
public:
	WeaponReplay(WeaponLane *p_lane, const SoldierState &p_base);
	void step(const Command &p_command, const SoldierState &p_before, const SoldierState &p_after);

private:
	WeaponLane *lane_ = nullptr;
	bool active_ = false;
	WeaponPredictionEntry current_;
};

}
