#pragma once

#include "bf3/host/game_server.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/soldier_motor.h"
#include "server_combat.h"
#include "server_combat_log.h"
#include "server_commands.h"
#include "server_outbox.h"
#include "server_slots.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bf3::host {

class SnapshotWriter {
public:
	SnapshotWriter(SlotTable &r_slots, CommandBuffers &r_commands, Combat &r_combat, CombatLog &r_log, const sim::SoldierMotor &p_motor, Outbox &r_outbox) :
			slots_(r_slots), commands_(r_commands), combat_(r_combat), log_(r_log), motor_(p_motor), outbox_(r_outbox) {}

	void configure(size_t p_slots) { seq_.assign(p_slots, 0); }
	void reset(size_t p_slot) { seq_[p_slot] = 0; }

	void send(sim::Tick p_tick, uint64_t p_now_ns, GameServerTickTimes &r_times);
	void stats(GameServerStats &r_stats) const;

private:
	void split_overflow(net::Snapshot &r_snapshot, size_t p_dropped);
	void send_extras(size_t p_slot, const net::Snapshot &p_snapshot);
	void send_shots(const ServerSlot &p_slot, const net::Snapshot &p_snapshot, Gunplay &r_gunplay);

	SlotTable &slots_;
	CommandBuffers &commands_;
	Combat &combat_;
	CombatLog &log_;
	const sim::SoldierMotor &motor_;
	Outbox &outbox_;

	std::vector<uint16_t> seq_;
	std::vector<net::OtherSoldier> overflow_;
	uint64_t main_mask_ = 0;
	std::vector<net::RemoteShot> remote_;

	uint64_t sent_ = 0;
	uint64_t bytes_ = 0;
	size_t max_bytes_ = 0;
	double last_encode_ms_ = 0;
};

}
