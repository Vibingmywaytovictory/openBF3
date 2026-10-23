#pragma once

#include "bf3/host/game_server.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/spawn.h"
#include "server_combat.h"
#include "server_outbox.h"
#include "server_slots.h"
#include "server_spawner.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace bf3::host {

class DevCommands {
public:
	DevCommands(SlotTable &r_slots, Spawner &r_spawner, Combat &r_combat, const sim::SoldierMotor &p_motor, Outbox &r_outbox,
			std::function<void(size_t)> p_reset_slot) :
			slots_(r_slots), spawner_(r_spawner), combat_(r_combat), motor_(p_motor), outbox_(r_outbox), reset_slot_(std::move(p_reset_slot)) {}

	void configure(const GameServerConfig &p_config, const sim::CollisionWorld &p_world, size_t p_slots);
	void reset(size_t p_slot) { per_slot_[p_slot] = PerSlot(); }
	void client_left(size_t p_slot);

	void report(uint32_t p_slot, uint32_t p_id, bool p_ok, std::string p_text);
	void handle_debug(size_t p_slot, const net::DebugRequest &p_request);
	void apply_debug(sim::Tick p_tick);
	void run_sweep(sim::Tick p_tick);
	void trace(size_t p_slot, sim::Tick p_tick);

private:
	struct PendingDebug {
		uint32_t slot = 0;
		net::DebugRequest request;
	};

	struct Sweep {
		bool active = false;
		uint32_t requester = 0;
		uint32_t id = 0;
		uint32_t ticks = 60;
		size_t next_point = 0;
		bool batch_active = false;
		sim::Tick batch_start = 0;
		std::vector<sim::SettleResult> results;
		std::vector<uint32_t> batch_points;
		std::vector<net::SpawnSweepRow> rows;
	};

	struct PerSlot {
		bool tracing = false;
		uint32_t trace_id = 0;
		uint32_t trace_ticks = 0;
		std::vector<sim::SoldierState> trace;
		size_t sweep_entry = 0;
	};

	void apply_target(const PendingDebug &p_debug, sim::Tick p_tick);

	SlotTable &slots_;
	Spawner &spawner_;
	Combat &combat_;
	const sim::SoldierMotor &motor_;
	Outbox &outbox_;
	std::function<void(size_t)> reset_slot_;
	const GameServerConfig *config_ = nullptr;
	const sim::CollisionWorld *world_ = nullptr;

	std::vector<PerSlot> per_slot_;
	std::deque<PendingDebug> queue_;
	Sweep sweep_;
};

}
