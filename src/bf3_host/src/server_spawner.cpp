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

bool Spawner::spawn(size_t p_slot, size_t p_point, sim::Tick p_tick) {
	const GameServerConfig &config = *config_;
	ServerSlot &s = slots_[p_slot];

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
		index = sim::choose_spawn(points, s.team, slots_.live_positions(p_slot), config.level->spawn_protection_radius,
				core::splitmix64_next(rng_));
		if (index == SIZE_MAX) {
			index = size_t(core::splitmix64_next(rng_) % points.size());
		}
	}
	if (index >= points.size()) {
		++failures_;
		return false;
	}
	const sim::SettleResult r = sim::settle_spawn(motor_, *world_, points[index], s.state.life + 1u, p_tick);
	++tick_spawns_;
	if (!r.ok) {
		std::fprintf(stderr, "GameServer: spawn point %u did not settle: %s (spawning there anyway)\n", points[index].index, r.failure.c_str());
	}
	s.state = r.state;
	s.spawned = true;
	s.want_spawn = false;
	s.spawn_index = uint32_t(index);
	s.last = sim::hold_command(s.state);
	s.own_events = uint16_t(s.own_events | s.state.events);
	on_spawn(p_slot, p_tick);
	if (s.remote) {
		net::Spawned m;
		m.slot = uint8_t(p_slot);
		m.spawn_index = points[index].index;
		m.state = s.state;
		describe_spawned(p_slot, m);
		outbox_.send(s.peer, net::kChannelReliable, net::encode_spawned(m));
	}
	return true;
}

core::Status Spawner::place(size_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose, sim::Tick p_tick) {
	if (p_slot >= slots_.size() || !slots_[p_slot].used || slots_[p_slot].dummy) {
		return core::make_error("place: no soldier slot " + std::to_string(p_slot));
	}
	ServerSlot &s = slots_[p_slot];
	if (s.remote) {
		return core::make_error("place: slot " + std::to_string(p_slot) + " is a client's");
	}
	if (!std::isfinite(p_position[0]) || !std::isfinite(p_position[1]) || !std::isfinite(p_position[2]) || size_t(p_pose) >= sim::kPoseCount) {
		return core::make_error("place: bad position or pose");
	}
	const bool was_alive = s.spawned && soldier_alive(s.state);

	s.state = motor_.spawn_state(p_position, p_yaw, was_alive ? s.state.life : s.state.life + 1u, p_tick, false);
	s.state.pose = p_pose;
	s.state.target_pose = p_pose;
	s.spawned = true;
	s.want_spawn = false;
	s.last = sim::hold_command(s.state);
	combat_.aim_target(p_slot, p_pose, p_yaw);
	if (!was_alive) {
		s.state.events = uint16_t(s.state.events | sim::soldier_events::kSpawned);
		s.own_events = uint16_t(s.own_events | s.state.events);
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

void Spawner::apply_orders(sim::Tick p_tick, std::span<const RulesOrder> p_orders) {
	for (const RulesOrder &o : p_orders) {
		if (o.slot >= slots_.size() || !slots_[o.slot].used || slots_[o.slot].dummy) {
			continue;
		}
		ServerSlot &s = slots_[o.slot];
		switch (o.kind) {
			case RulesOrder::Kind::Spawn:
				if (!s.spawned || !soldier_alive(s.state)) {
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
				log_.revive(e);
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
	ServerSlot &s = slots_[p_slot];
	if (s.spawned && soldier_alive(s.state)) {
		return core::make_error("revive: slot " + std::to_string(p_slot) + " is alive");
	}
	if (!std::isfinite(p_position[0]) || !std::isfinite(p_position[1]) || !std::isfinite(p_position[2]) || !std::isfinite(p_health) || p_health <= 0.0f) {
		return core::make_error("revive: bad position or health");
	}

	s.state = motor_.spawn_state(p_position, p_yaw, s.state.life + 1u, p_tick, false);
	s.state.health = std::min(p_health, s.state.health);
	s.state.events = uint16_t(s.state.events | sim::soldier_events::kSpawned);
	s.spawned = true;
	s.want_spawn = false;
	s.respawn_at = 0;
	s.last = sim::hold_command(s.state);
	s.own_events = uint16_t(s.own_events | s.state.events);
	on_spawn(p_slot, p_tick);
	if (s.remote) {
		net::Spawned m;
		m.slot = uint8_t(p_slot);
		m.spawn_index = 0;
		m.state = s.state;
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
	ServerSlot &s = slots_[p_slot];
	s.spawned = false;
	s.want_spawn = false;
	s.respawn_at = 0;
	combat_.disarm(p_slot);
}

void Spawner::on_spawn(size_t p_slot, sim::Tick p_tick) {
	if (p_slot >= slots_.size()) {
		return;
	}
	ServerSlot &s = slots_[p_slot];
	if (const std::optional<uint8_t> team = combat_.take_next_team(p_slot)) {
		s.team = *team;
	}
	if (TdmRules *rules = match_.rules(); rules != nullptr) {
		s.team = rules->team_of(uint8_t(p_slot));
		if (const uint8_t k = rules->kit_of(uint8_t(p_slot)); k != kKeepKit) {
			combat_.set_kit(p_slot, k);
		}
		rules->on_spawned(uint8_t(p_slot), p_tick);
	} else if (!s.remote) {
		combat_.set_kit(p_slot, uint8_t(p_slot % kKitCount));
	}
	if (!s.dummy) {
		combat_.arm(p_slot, uint8_t(std::clamp(s.team, 0, 255)), p_tick);
	}
}

void Spawner::describe_spawned(size_t p_slot, net::Spawned &r_message) const {
	r_message.kit = combat_.kit(p_slot);
	r_message.team = uint8_t(std::clamp(slots_[p_slot].team, 0, int(net::kMaxTeam)));
}

}
