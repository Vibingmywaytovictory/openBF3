#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "server_combat.h"
#include "server_combat_log.h"
#include "server_match.h"
#include "server_outbox.h"
#include "server_slots.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::host {

class Spawner {
public:
	Spawner(SlotTable &r_slots, MatchControl &r_match, Combat &r_combat, CombatLog &r_log, const sim::SoldierMotor &p_motor, Outbox &r_outbox) :
			slots_(r_slots), match_(r_match), combat_(r_combat), log_(r_log), motor_(p_motor), outbox_(r_outbox) {}

	void configure(const GameServerConfig &p_config, const sim::CollisionWorld &p_world, const ServerView &p_view);
	void seed(uint64_t p_rng) { rng_ = p_rng; }

	void begin_tick() { tick_spawns_ = 0; }
	uint32_t tick_spawns() const { return tick_spawns_; }
	uint64_t failures() const { return failures_; }

	bool spawn(size_t p_slot, size_t p_point, sim::Tick p_tick);
	core::Status place(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose, sim::Tick p_tick);
	core::Status queue_place(uint32_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose);
	void place_queued(sim::Tick p_tick);
	void apply_orders(sim::Tick p_tick, std::span<const RulesOrder> p_orders);

private:
	struct Placement {
		size_t slot = 0;
		sim::Vec3 position{};
		uint16_t yaw = 0;
		sim::Pose pose = sim::Pose::Stand;
	};

	core::Status revive(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, float p_health, sim::Tick p_tick);
	void unspawn(size_t p_slot);
	void on_spawn(size_t p_slot, sim::Tick p_tick);
	void describe_spawned(size_t p_slot, net::Spawned &r_message) const;

	SlotTable &slots_;
	MatchControl &match_;
	Combat &combat_;
	CombatLog &log_;
	const sim::SoldierMotor &motor_;
	Outbox &outbox_;
	const GameServerConfig *config_ = nullptr;
	const sim::CollisionWorld *world_ = nullptr;
	const ServerView *view_ = nullptr;

	uint64_t rng_ = 1;
	std::vector<Placement> placements_;
	uint64_t failures_ = 0;
	uint32_t tick_spawns_ = 0;
};

}
