#include "server_spawner.h"

#include "bf3/core/util/splitmix64.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/kits.h"
#include "bf3/sim/spawn.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace bf3::host {

void Spawner::configure(const GameServerConfig &p_config, const sim::CollisionWorld &p_world, const ServerView &p_view) {
	config_ = &p_config;
	world_ = &p_world;
	view_ = &p_view;
	rng_ = p_config.seed;
}

void Spawner::ready(size_t p_slot) {
	if (!soldiers_[p_slot].spawned && !match_.waits_for_deploy(p_slot)) {
		respawns_[p_slot].wanted = true;
	}
}

void Spawner::died(size_t p_slot, sim::Tick p_tick) {
	const std::optional<uint32_t> respawn = match_.policy().respawn_tick(uint8_t(p_slot), p_tick);
	respawns_[p_slot].wanted = respawn.has_value();
	respawns_[p_slot].at = respawn.value_or(0);
}

void Spawner::killed(size_t p_slot, sim::Tick p_tick) {
	if (p_slot >= slots_.size()) {
		return;
	}
	const Seat &s = slots_[p_slot];
	if (s.used && !s.dummy && !respawns_[p_slot].wanted) {
		const std::optional<uint32_t> at = match_.policy().respawn_tick(uint8_t(p_slot), p_tick);
		if (at) {
			respawns_[p_slot].wanted = true;
			respawns_[p_slot].at = *at;
		}
	}
}

void Spawner::spawn_due(sim::Tick p_tick) {
	for (size_t i = 0; i < slots_.size(); ++i) {
		const Seat &s = slots_[i];
		if (s.used && !s.dummy && respawns_[i].wanted && (!soldiers_[i].spawned || p_tick >= respawns_[i].at)) {
			spawn(i, SIZE_MAX, p_tick);
		}
	}
}

std::vector<sim::Vec3> Spawner::live_positions(size_t p_except) const {
	std::vector<sim::Vec3> out;
	for (size_t i = 0; i < slots_.size(); ++i) {
		const Seat &s = slots_[i];
		if (i != p_except && s.used && !s.dummy && soldiers_.alive(i)) {
			out.push_back(soldiers_[i].state.position);
		}
	}
	return out;
}

bool Spawner::spawn(size_t p_slot, size_t p_point, sim::Tick p_tick) {
	const GameServerConfig &config = *config_;
	const Seat &s = slots_[p_slot];

	const std::vector<sim::SpawnPoint> &points =
			s.bot && config.bots != nullptr && config.bots->spawn_in_main && config.bots->level != nullptr ? config.bots->level->spawns.points
																											 : config.level->spawns;
	if (points.empty()) {
		++failures_;
		return false;
	}
	size_t index = p_point;
	if (index == SIZE_MAX) {
		index = match_.choose_spawn(p_slot, points, *view_, p_tick);
	}
	if (index == SIZE_MAX) {
		index = sim::choose_spawn(points, s.team, live_positions(p_slot), config.level->spawn_protection_radius,
				core::splitmix64_next(rng_));
		if (index == SIZE_MAX) {
			index = size_t(core::splitmix64_next(rng_) % points.size());
		}
	}
	if (index >= points.size()) {
		++failures_;
		return false;
	}
	const sim::SettleResult r = sim::settle_spawn(motor_, *world_, points[index], soldiers_[p_slot].state.life + 1u, p_tick);
	++tick_spawns_;
	if (!r.ok) {
		std::fprintf(stderr, "GameServer: spawn point %u did not settle: %s (spawning there anyway)\n", points[index].index, r.failure.c_str());
	}
	soldiers_.enter(p_slot, r.state);
	respawns_[p_slot].wanted = false;
	on_spawn(p_slot, p_tick);
	if (s.remote) {
		net::Spawned m;
		m.slot = uint8_t(p_slot);
		m.spawn_index = points[index].index;
		m.state = soldiers_[p_slot].state;
		describe_spawned(p_slot, m);
		outbox_.send(s.peer, net::kChannelReliable, net::encode_spawned(m));
	}
	return true;
}

core::Status Spawner::place(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose, sim::Tick p_tick) {
	if (p_slot >= slots_.size() || !slots_[p_slot].used || slots_[p_slot].dummy) {
		return core::make_error("place: no soldier slot " + std::to_string(p_slot));
	}
	if (slots_[p_slot].remote) {
		return core::make_error("place: slot " + std::to_string(p_slot) + " is a client's");
	}
	if (!std::isfinite(p_position[0]) || !std::isfinite(p_position[1]) || !std::isfinite(p_position[2]) || size_t(p_pose) >= sim::kPoseCount) {
		return core::make_error("place: bad position or pose");
	}
	const bool was_alive = soldiers_.alive(p_slot);
	const uint32_t life = soldiers_[p_slot].state.life;

	sim::SoldierState state = motor_.spawn_state(p_position, p_yaw, was_alive ? life : life + 1u, p_tick, false);
	state.pose = p_pose;
	state.target_pose = p_pose;
	respawns_[p_slot].wanted = false;
	combat_.aim_target(p_slot, p_pose, p_yaw);
	if (was_alive) {
		soldiers_.replace(p_slot, state);
	} else {
		state.events = uint16_t(state.events | sim::soldier_events::kSpawned);
		soldiers_.enter(p_slot, state);
		on_spawn(p_slot, p_tick);
	}
	return core::Status::success();
}

core::Status Spawner::queue_place(uint32_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose) {
	if (p_slot >= slots_.size() || !slots_[p_slot].used || slots_[p_slot].dummy) {
		return core::make_error("place_soldier: no soldier slot " + std::to_string(p_slot));
	}
	if (slots_[p_slot].remote) {
		return core::make_error("place_soldier: slot " + std::to_string(p_slot) + " is a client's");
	}
	if (!std::isfinite(p_position[0]) || !std::isfinite(p_position[1]) || !std::isfinite(p_position[2]) || size_t(p_pose) >= sim::kPoseCount) {
		return core::make_error("place_soldier: non-finite position or bad pose");
	}
	if (placements_.size() >= slots_.size() * 2) {
		return core::make_error("place_soldier: too many placements this tick");
	}
	placements_.push_back({ size_t(p_slot), p_position, p_yaw, p_pose });
	return core::Status::success();
}

void Spawner::place_queued(sim::Tick p_tick) {
	std::vector<Placement> placements = std::move(placements_);
	placements_.clear();
	for (const Placement &p : placements) {
		const core::Status st = place(p.slot, p.position, p.yaw, p.pose, p_tick);
		if (!st.ok()) {
			std::fprintf(stderr, "GameServer: place_soldier: %s\n", st.error().message.c_str());
		}
	}
}

void Spawner::apply_orders(sim::Tick p_tick, std::span<const RulesOrder> p_orders, CombatEventSink &r_log) {
	for (const RulesOrder &o : p_orders) {
		if (o.slot >= slots_.size() || !slots_[o.slot].used || slots_[o.slot].dummy) {
			continue;
		}
		switch (o.kind) {
			case RulesOrder::Kind::Spawn:
				if (!soldiers_.alive(o.slot)) {
					spawn(o.slot, SIZE_MAX, p_tick);
				}
				break;
			case RulesOrder::Kind::Revive: {
				const core::Status st = revive(o.slot, o.position, o.yaw, o.health, p_tick);
				if (!st.ok()) {
					std::fprintf(stderr, "GameServer: %s\n", st.error().message.c_str());
					break;
				}
				net::ReviveEvent e;
				e.reviver = o.reviver;
				e.revived = o.slot;
				e.tick = p_tick;
				r_log.revive(e);
				break;
			}
			case RulesOrder::Kind::Unspawn:
				unspawn(o.slot);
				break;
		}
	}
}

core::Status Spawner::revive(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, float p_health, sim::Tick p_tick) {
	if (p_slot >= slots_.size() || !slots_[p_slot].used || slots_[p_slot].dummy) {
		return core::make_error("revive: no soldier slot " + std::to_string(p_slot));
	}
	if (soldiers_.alive(p_slot)) {
		return core::make_error("revive: slot " + std::to_string(p_slot) + " is alive");
	}
	if (!std::isfinite(p_position[0]) || !std::isfinite(p_position[1]) || !std::isfinite(p_position[2]) || !std::isfinite(p_health) || p_health <= 0.0f) {
		return core::make_error("revive: bad position or health");
	}

	sim::SoldierState state = motor_.spawn_state(p_position, p_yaw, soldiers_[p_slot].state.life + 1u, p_tick, false);
	state.health = std::min(p_health, state.health);
	state.events = uint16_t(state.events | sim::soldier_events::kSpawned);
	soldiers_.enter(p_slot, state);
	respawns_[p_slot] = Respawn();
	on_spawn(p_slot, p_tick);
	const Seat &s = slots_[p_slot];
	if (s.remote) {
		net::Spawned m;
		m.slot = uint8_t(p_slot);
		m.spawn_index = 0;
		m.state = soldiers_[p_slot].state;
		m.revived = true;
		describe_spawned(p_slot, m);
		outbox_.send(s.peer, net::kChannelReliable, net::encode_spawned(m));
	}
	return core::Status::success();
}

void Spawner::unspawn(size_t p_slot) {
	if (p_slot >= slots_.size() || !slots_[p_slot].used || slots_[p_slot].dummy) {
		return;
	}
	soldiers_.leave(p_slot);
	respawns_[p_slot] = Respawn();
	combat_.disarm(p_slot);
}

void Spawner::on_spawn(size_t p_slot, sim::Tick p_tick) {
	if (p_slot >= slots_.size()) {
		return;
	}
	if (const std::optional<uint8_t> team = combat_.take_next_team(p_slot)) {
		slots_.assign_team(p_slot, *team);
	}
	if (TdmRules *rules = match_.rules(); rules != nullptr) {
		slots_.assign_team(p_slot, rules->team_of(uint8_t(p_slot)));
		if (const uint8_t k = rules->kit_of(uint8_t(p_slot)); k != kKeepKit) {
			combat_.set_kit(p_slot, k);
		}
		rules->on_spawned(uint8_t(p_slot), p_tick);
	} else if (!slots_[p_slot].remote) {
		combat_.set_kit(p_slot, uint8_t(p_slot % kKitCount));
	}
	if (!slots_[p_slot].dummy) {
		combat_.arm(p_slot, uint8_t(std::clamp(slots_[p_slot].team, 0, 255)), p_tick);
	}
}

void Spawner::describe_spawned(size_t p_slot, net::Spawned &r_message) const {
	r_message.kit = combat_.kit(p_slot);
	r_message.defs = combat_.spawned_defs(p_slot);
	r_message.team = uint8_t(std::clamp(slots_[p_slot].team, 0, int(net::kMaxTeam)));
}

}
