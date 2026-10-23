#include "server_combat.h"

#include "bf3/host/kits.h"

#include <algorithm>
#include <utility>

namespace bf3::host {

void Combat::configure(size_t p_slots, const sim::StaticWorldShapes &p_static_world) {
	per_slot_.resize(p_slots);
	frame_ = shot_frame(p_static_world);
}

core::Status Combat::create(const SharedLevel &p_level, const GunplayConfig &p_config, uint64_t p_server_seed, const sim::CollisionWorld &p_world) {
	core::Result<std::unique_ptr<Gunplay>> created = Gunplay::create(p_level, p_config);
	if (!created) {
		return core::error_at("GameServer", created.error());
	}
	gunplay_ = std::move(*created);
	gunplay_->set_world(p_world);
	gunplay_->set_match_seed(p_config.match_seed != 0 ? p_config.match_seed : derive_match_seed(p_server_seed, p_level.gunplay->level_hash));
	return core::Status::success();
}

void Combat::ack(size_t p_slot, const net::CommandPacket &p_packet) {
	if (gunplay_ == nullptr) {
		return;
	}
	gunplay_->history().ack(uint8_t(p_slot), p_packet.newest_snapshot, p_packet.snapshot_ack_bits, p_packet.others_ack_bits);
}

void Combat::note_command(size_t p_slot, const sim::Command &p_command) {
	if (gunplay_ == nullptr) {
		return;
	}
	gunplay_->note_command(uint8_t(p_slot), p_command, gunplay_->history().ack_state(uint8_t(p_slot)));
}

void Combat::step_soldier(size_t p_slot, const sim::Command &p_command, const sim::SoldierState &p_before, const sim::SoldierState &p_after) {
	if (gunplay_ == nullptr) {
		return;
	}
	gunplay_->step_soldier(uint8_t(p_slot), p_command, p_before, p_after);
}

void Combat::resolve(sim::Tick p_tick, const SlotTable &p_slots, CombatEventSink &r_sink) {
	tick_states_.assign(p_slots.size(), sim::SoldierState());
	for (size_t i = 0; i < p_slots.size(); ++i) {
		if (p_slots[i].used && p_slots[i].spawned && !p_slots[i].dummy) {
			tick_states_[i] = p_slots[i].state;
		}
	}
	gunplay_->step_projectiles(p_tick, tick_states_);
	gunplay_->apply_rules(p_tick, tick_states_, r_sink);
}

void Combat::restore(SlotTable &r_slots) const {
	for (size_t i = 0; i < r_slots.size(); ++i) {
		if (r_slots[i].used && r_slots[i].spawned && !r_slots[i].dummy) {
			r_slots[i].state = tick_states_[i];
		}
	}
}

float Combat::killer_health(uint8_t p_killer, uint8_t p_victim) const {
	const bool fall = p_killer == p_victim || p_killer >= tick_states_.size();
	return fall ? 0.0f : tick_states_[p_killer].health;
}

std::optional<uint8_t> Combat::take_next_team(size_t p_slot) {
	PerSlot &ps = per_slot_[p_slot];
	if (ps.next_team == 0) {
		return std::nullopt;
	}
	const uint8_t team = ps.next_team;
	ps.next_team = 0;
	return team;
}

void Combat::arm(size_t p_slot, uint8_t p_team, sim::Tick p_tick) {
	if (gunplay_ != nullptr) {
		gunplay_->on_spawn(uint8_t(p_slot), per_slot_[p_slot].kit, p_team, p_tick);
	}
}

void Combat::disarm(size_t p_slot) {
	if (gunplay_ != nullptr) {
		gunplay_->clear(uint8_t(p_slot));
	}
}

void Combat::aim_target(size_t p_slot, sim::Pose p_pose, uint16_t p_yaw) {
	if (per_slot_[p_slot].target != nullptr) {
		TargetBotConfig c;
		c.pose = p_pose;
		c.yaw = p_yaw;
		per_slot_[p_slot].target->set(c);
	}
}

std::optional<uint8_t> Combat::client_joined(size_t p_slot, bool p_dev_commands, net::MatchInfo &r_message) {
	PerSlot &ps = per_slot_[p_slot];
	ps = PerSlot();
	if (gunplay_ == nullptr) {
		return std::nullopt;
	}
	Gunplay &g = *gunplay_;
	ps.kit = g.config().player_kit < kKitCount ? g.config().player_kit : 0;
	std::optional<uint8_t> team;
	if (p_dev_commands && (g.config().player_team == 1 || g.config().player_team == 2)) {
		team = g.config().player_team;
	}
	g.history().client_reset(uint8_t(p_slot));
	g.clear(uint8_t(p_slot));
	r_message.match_seed = g.match_seed();
	r_message.weapon_hash = g.weapon_hash();
	r_message.weapon_flags = g.weapon_flags();
	r_message.kit_hash = g.kit_hash();
	r_message.max_rewind_ticks = uint8_t(std::min<uint32_t>(g.config().lag.max_rewind_ticks, 255u));
	return team;
}

void Combat::client_left(size_t p_slot) {
	per_slot_[p_slot] = PerSlot();
	if (gunplay_ != nullptr) {
		gunplay_->history().client_reset(uint8_t(p_slot));
		gunplay_->clear(uint8_t(p_slot));
	}
}

void Combat::select_kit(size_t p_slot, const net::KitSelect &p_select, bool p_dev_commands) {
	if (p_select.kit >= kKitCount || p_select.team > net::kMaxTeam) {
		refuse_kit_select();
		return;
	}
	PerSlot &ps = per_slot_[p_slot];
	bool team_refused = false;
	ps.kit = p_select.kit;
	if (p_select.team != 0) {
		if (p_dev_commands) {
			ps.next_team = p_select.team;
		} else {
			team_refused = true;
		}
	}
	if (gunplay_ != nullptr) {
		gunplay_->count_kit_select(true, team_refused);
	}
}

void Combat::refuse_kit_select() {
	if (gunplay_ != nullptr) {
		gunplay_->count_kit_select(false, false);
	}
}

void Combat::record_tick(sim::Tick p_tick, const SlotTable &p_slots) {
	if (gunplay_ == nullptr) {
		return;
	}
	records_.clear();
	for (size_t i = 0; i < p_slots.size(); ++i) {
		PerSlot &ps = per_slot_[i];
		ps.record_valid = false;
		const ServerSlot &s = p_slots[i];
		if (s.used && s.spawned && soldier_alive(s.state)) {
			ps.record = gunplay_->other_record(uint8_t(i), s.state);
			ps.record_valid = true;
			records_.push_back(ps.record);
		}
	}
	gunplay_->history().record_tick(p_tick, records_);
}

void Combat::fill_snapshot(size_t p_slot, net::Snapshot &r_snapshot) const {
	if (gunplay_ == nullptr) {
		return;
	}
	if (r_snapshot.own && gunplay_->armed(uint8_t(p_slot))) {
		r_snapshot.own_weapons = net::weapons_wire_view(gunplay_->weapons(uint8_t(p_slot)), r_snapshot.server_tick);
	}
	for (net::OtherSoldier &o : r_snapshot.others) {
		if (o.slot < per_slot_.size() && per_slot_[o.slot].record_valid) {
			o = per_slot_[o.slot].record;
		}
	}
}

void Combat::trace_tick(size_t p_slot, bool p_first, sim::Tick p_tick) {
	if (gunplay_ == nullptr || p_slot >= per_slot_.size()) {
		return;
	}
	PerSlot &ps = per_slot_[p_slot];
	if (p_first) {
		ps.trace.clear();
		ps.trace_dropped = 0;
	}
	uint32_t dropped = 0;
	ps.trace.push_back(gunplay_->trace_tick(uint8_t(p_slot), p_tick, dropped));
	ps.trace_dropped += dropped;
	gunplay_->count_trace_dropped(dropped);
}

void Combat::take_trace(size_t p_slot, net::DebugReport &r_report) {
	if (gunplay_ == nullptr || p_slot >= per_slot_.size()) {
		return;
	}
	PerSlot &ps = per_slot_[p_slot];
	r_report.trace_gunplay = std::move(ps.trace);
	r_report.trace_dropped = ps.trace_dropped;
	ps.trace.clear();
	ps.trace_dropped = 0;
}

void Combat::stats(GameServerStats &r_stats) const {
	if (gunplay_ == nullptr) {
		return;
	}
	const Gunplay &g = *gunplay_;
	r_stats.gunplay_active = true;
	r_stats.gunplay = g.stats();
	r_stats.match_seed = g.match_seed();
	r_stats.weapon_hash = g.weapon_hash();
	r_stats.kit_hash = g.kit_hash();
	r_stats.weapon_flags = g.weapon_flags();
}

}
