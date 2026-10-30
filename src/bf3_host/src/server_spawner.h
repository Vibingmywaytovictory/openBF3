#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "server_combat.h"
#include "server_logic_sync.h"
#include "server_match.h"
#include "server_outbox.h"
#include "server_slots.h"
#include "server_soldiers.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::host {

class Spawner {
public:
	Spawner(SlotTable &r_slots, Soldiers &r_soldiers, MatchControl &r_match, Combat &r_combat,
			const sim::SoldierMotor &p_motor, Outbox &r_outbox, ServerLogicSync &r_logic) :
			slots_(r_slots), soldiers_(r_soldiers), match_(r_match), combat_(r_combat), motor_(p_motor),
			outbox_(r_outbox), logic_(r_logic) {}

	void configure(const GameServerConfig &p_config, const sim::CollisionWorld &p_world, const ServerView &p_view);
	void resize(size_t p_slots) { respawns_.assign(p_slots, Respawn()); }
	void reset(size_t p_slot) { respawns_[p_slot] = Respawn(); }
	void seed(uint64_t p_rng) { rng_ = p_rng; }

	void begin_tick() { tick_spawns_ = 0; }
	uint32_t tick_spawns() const { return tick_spawns_; }
	uint64_t failures() const { return failures_; }

	void ready(size_t p_slot);
	void died(size_t p_slot, sim::Tick p_tick);
	void killed(size_t p_slot, sim::Tick p_tick);
	void spawn_due(sim::Tick p_tick);

	bool spawn(size_t p_slot, size_t p_point, sim::Tick p_tick);
	core::Status place(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose, sim::Tick p_tick);
	core::Status queue_place(uint32_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose);
	void place_queued(sim::Tick p_tick);
	void apply_orders(sim::Tick p_tick, std::span<const RulesOrder> p_orders, CombatEventSink &r_log);

private:
	struct Placement {
		size_t slot = 0;
		sim::Vec3 position{};
		uint16_t yaw = 0;
		sim::Pose pose = sim::Pose::Stand;
	};

	struct Respawn {
		bool wanted = false;
		sim::Tick at = 0;
	};

	void enter_at(size_t p_slot, const sim::SpawnPoint &p_point, sim::Tick p_tick);
	core::Status revive(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, float p_health, sim::Tick p_tick);
	void unspawn(size_t p_slot);
	void on_spawn(size_t p_slot, sim::Tick p_tick);
	void describe_spawned(size_t p_slot, net::Spawned &r_message) const;
	void announce_gear(size_t p_slot);
	std::vector<sim::Vec3> live_positions(size_t p_except) const;
	std::span<const sim::SoldierState> live_states();

	SlotTable &slots_;
	Soldiers &soldiers_;
	MatchControl &match_;
	Combat &combat_;
	const sim::SoldierMotor &motor_;
	Outbox &outbox_;
	ServerLogicSync &logic_;
	const GameServerConfig *config_ = nullptr;
	const sim::CollisionWorld *world_ = nullptr;
	const ServerView *view_ = nullptr;

	uint64_t rng_ = 1;
	std::vector<Respawn> respawns_;
	std::vector<Placement> placements_;
	std::vector<sim::SoldierState> live_;
	uint64_t failures_ = 0;
	uint32_t tick_spawns_ = 0;
};

}
