#pragma once

#include "bf3/host/game_server.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/soldier_motor.h"
#include "server_combat.h"
#include "server_combat_log.h"
#include "server_commands.h"
#include "server_ghosts.h"
#include "server_match.h"
#include "server_outbox.h"
#include "server_slots.h"
#include "server_soldiers.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bf3::host {

class SnapshotWriter {
public:
	SnapshotWriter(const SlotTable &p_slots, Soldiers &r_soldiers, CommandBuffers &r_commands, Combat &r_combat,
			CombatLog &r_log, const sim::SoldierMotor &p_motor, Outbox &r_outbox, const MatchControl &p_match) :
			slots_(p_slots), soldiers_(r_soldiers), commands_(r_commands), combat_(r_combat), log_(r_log),
			motor_(p_motor), outbox_(r_outbox), match_(p_match) {}

	void configure(size_t p_slots, uint32_t p_budget_bytes) {
		seq_.assign(p_slots, 0);
		ghosts_.configure(p_slots, int64_t(p_budget_bytes));
	}
	void reset(size_t p_slot) {
		seq_[p_slot] = 0;
		ghosts_.reset(p_slot);
	}
	void ack(size_t p_slot, const net::CommandPacket &p_packet) { ghosts_.ack(p_slot, p_packet); }

	void send(sim::Tick p_tick, uint64_t p_now_ns, GameServerTickTimes &r_times);
	void stats(GameServerStats &r_stats) const;

private:
	void gather(sim::Tick p_tick);
	GhostViewer viewer(size_t p_slot) const;
	size_t send_overflow(size_t p_slot, const net::Snapshot &p_snapshot, size_t p_carried, int64_t p_room,
			std::vector<net::OtherSoldier> &r_sent);
	void send_extras(size_t p_slot, const net::Snapshot &p_snapshot, std::span<const net::OtherSoldier> p_main,
			std::span<const net::OtherSoldier> p_others);
	void send_shots(size_t p_slot, const net::Snapshot &p_snapshot, std::vector<uint8_t> p_wanted, size_t p_cut,
			int64_t p_room, Gunplay &r_gunplay);
	std::vector<uint8_t> shot_events(
			size_t p_slot, const net::Snapshot &p_snapshot, size_t p_max_bytes, size_t &r_cut) const;

	const SlotTable &slots_;
	Soldiers &soldiers_;
	CommandBuffers &commands_;
	Combat &combat_;
	CombatLog &log_;
	const sim::SoldierMotor &motor_;
	Outbox &outbox_;
	const MatchControl &match_;

	std::vector<uint16_t> seq_;
	GhostScheduler ghosts_;
	std::vector<GhostCandidate> candidates_;
	std::vector<int> candidate_of_;
	GhostPlan plan_;
	std::vector<net::RemoteShot> remote_;

	uint64_t sent_ = 0;
	uint64_t bytes_ = 0;
	size_t max_bytes_ = 0;
	double last_encode_ms_ = 0;
};

}
