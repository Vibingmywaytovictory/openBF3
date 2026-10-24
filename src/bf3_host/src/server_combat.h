#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_load.h"
#include "bf3/host/target_bots.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "server_slots.h"
#include "server_soldiers.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bf3::host {

class Combat {
public:
	void configure(size_t p_slots, const sim::StaticWorldShapes &p_static_world);
	core::Status create(const SharedLevel &p_level, const GunplayConfig &p_config, uint64_t p_server_seed, const sim::CollisionWorld &p_world);

	bool on() const { return gunplay_ != nullptr; }
	Gunplay *gunplay() { return gunplay_.get(); }
	const Gunplay *gunplay() const { return gunplay_.get(); }
	const net::ShotQuantization &frame() const { return frame_; }

	void ack(size_t p_slot, const net::CommandPacket &p_packet);
	void note_command(size_t p_slot, const sim::Command &p_command);
	void step_soldier(size_t p_slot, const sim::Command &p_command, const sim::SoldierState &p_before, const sim::SoldierState &p_after);

	void resolve(sim::Tick p_tick, const SlotTable &p_slots, const Soldiers &p_soldiers, CombatEventSink &r_sink);
	std::span<sim::SoldierState> tick_states() { return tick_states_; }
	float killer_health(uint8_t p_killer, uint8_t p_victim) const;

	uint8_t kit(size_t p_slot) const { return per_slot_[p_slot].kit; }
	void set_kit(size_t p_slot, uint8_t p_kit) { per_slot_[p_slot].kit = p_kit; }
	std::optional<uint8_t> take_next_team(size_t p_slot);
	void arm(size_t p_slot, uint8_t p_team, sim::Tick p_tick);
	void disarm(size_t p_slot);
	void set_target(size_t p_slot, TargetBotSource *p_target) { per_slot_[p_slot].target = p_target; }
	void aim_target(size_t p_slot, sim::Pose p_pose, uint16_t p_yaw);

	std::optional<uint8_t> client_joined(size_t p_slot, bool p_dev_commands, net::MatchInfo &r_message);
	void client_left(size_t p_slot);
	void select_kit(size_t p_slot, const net::KitSelect &p_select, bool p_dev_commands);
	void refuse_kit_select();

	void record_tick(sim::Tick p_tick, const SlotTable &p_slots, const Soldiers &p_soldiers);
	void fill_snapshot(size_t p_slot, net::Snapshot &r_snapshot) const;

	void trace_tick(size_t p_slot, bool p_first, sim::Tick p_tick);
	void take_trace(size_t p_slot, net::DebugReport &r_report);

	void stats(GameServerStats &r_stats) const;

private:
	struct PerSlot {
		uint8_t kit = 0;
		uint8_t next_team = 0;
		TargetBotSource *target = nullptr;
		std::vector<net::TraceGunplayTick> trace;
		uint32_t trace_dropped = 0;
		net::OtherSoldier record;
		bool record_valid = false;
	};

	std::unique_ptr<Gunplay> gunplay_;
	net::ShotQuantization frame_;
	std::vector<PerSlot> per_slot_;
	std::vector<sim::SoldierState> tick_states_;
	std::vector<net::OtherSoldier> records_;
};

}
