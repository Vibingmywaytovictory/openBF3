#include "client_match.h"

#include <algorithm>

namespace bf3::host {

bool ClientMatch::datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d) {
	switch (p_type) {
		case net::MessageType::MatchState: {
			core::Result<net::MatchState> m = net::decode_match_state(p_d.bytes);
			if (!m) {
				++bad_tdm_datagrams_;
				return true;
			}
			match_ = *m;
			valid_ = true;
			++match_states_;
			return true;
		}
		case net::MessageType::PlayerList: {
			core::Result<net::PlayerList> l = net::decode_player_list(p_d.bytes);
			if (!l) {
				++bad_tdm_datagrams_;
				return true;
			}
			apply_player_list(players_, *l);
			++player_lists_;
			return true;
		}
		case net::MessageType::SoldierGear: {
			core::Result<net::SoldierGear> g = net::decode_soldier_gear(p_d.bytes);
			if (!g) {
				++bad_tdm_datagrams_;
				return true;
			}
			gear_[g->slot] = *g;
			++soldier_gears_;
			return true;
		}
		case net::MessageType::SquadOrder: {
			core::Result<net::SquadOrder> o = net::decode_squad_order(p_d.bytes);
			if (!o || !apply_squad_order(orders_, *o)) {
				++bad_tdm_datagrams_;
			}
			return true;
		}
		case net::MessageType::Deployables: {
			core::Result<net::Deployables> d = net::decode_deployables(p_d.bytes);
			if (!d) {
				++bad_tdm_datagrams_;
				return true;
			}
			deployables(*d);
			return true;
		}
		case net::MessageType::SpotStates: {
			core::Result<net::SpotStates> s = net::decode_spot_states(p_d.bytes);
			if (!s) {
				++bad_tdm_datagrams_;
				return true;
			}
			if (s->full) {
				spots_ = kNoSpots;
			}
			for (const net::SpotRow &row : s->rows) {
				spots_[row.slot] = row;
			}
			++spot_state_messages_;
			return true;
		}
		case net::MessageType::OrderNotice: {
			core::Result<net::OrderNotice> o = net::decode_order_notice(p_d.bytes);
			if (!o) {
				++bad_tdm_datagrams_;
				return true;
			}
			own_order_.notice = *o;
			++own_order_.serial;
			return true;
		}
		case net::MessageType::ConquestState: {
			core::Result<net::ConquestState> c = net::decode_conquest_state(p_d.bytes);
			if (!c) {
				++bad_tdm_datagrams_;
				return true;
			}
			conquest_ = *c;
			conquest_valid_ = true;
			++conquest_states_;
			return true;
		}
		case net::MessageType::SpawnAnswer: {
			core::Result<net::SpawnAnswer> a = net::decode_spawn_answer(p_d.bytes);
			if (!a) {
				++bad_tdm_datagrams_;
				return true;
			}
			spawn_answer_ = *a;
			++spawn_answers_;
			return true;
		}
		case net::MessageType::LogicEvents:
		case net::MessageType::FullSync:
		case net::MessageType::LogicDelta:
			logic_.datagram(p_type, p_d.bytes);
			return true;
		default:
			return false;
	}
}

const char *ClientMatch::check_rules(const net::MatchInfo &p_match, const SharedLevel &p_level) {
	const char *refusal = rules_refusal(p_match, p_level.logic != nullptr, p_level.logic_digest);
	if (refusal != nullptr) {
		rules_mismatch_ = true;
	}
	return refusal;
}

void ClientMatch::combat(const net::CombatEvents &p_events, bool p_have_own, uint32_t p_own_life) {
	for (const net::DeathEvent &d : p_events.deaths) {
		if (d.victim == own_slot_ && p_have_own) {
			death_valid_ = true;
			death_life_ = p_own_life;
			respawn_tick_ = d.respawn_tick;
			revive_until_ = d.revive_until;
		}
	}
	for (const net::ReviveEvent &v : p_events.revives) {
		if (!valid_slot(v.reviver) || !valid_slot(v.revived)) {
			++bad_events_;
			continue;
		}
		PresentEvent ev;
		ev.kind = PresentKind::Revive;
		ev.timeline = PresentTimeline::Render;
		ev.slot = v.reviver;
		ev.aux = v.revived;
		ev.id = v.tick;
		ev.time = sim::weapons::tick_end(v.tick);
		ev.flags = uint8_t((v.reviver == own_slot_ ? present_flags::kOwnShooter : 0u) | (v.revived == own_slot_ ? present_flags::kOwnVictim : 0u));
		events_.push(ev);
		++revives_;
	}
}

void ClientMatch::deployables(const net::Deployables &p_message) {
	++deployable_messages_;
	for (const uint16_t id : p_message.removed) {
		const auto it = std::find_if(
				deployables_.begin(), deployables_.end(), [&](const net::DeployableWire &d) { return d.id == id; });
		if (it == deployables_.end()) {
			++bad_deployables_;
			continue;
		}
		PresentEvent ev;
		ev.kind = PresentKind::GadgetRemove;
		ev.timeline = PresentTimeline::Immediate;
		ev.slot = it->owner;
		ev.def = it->def;
		ev.id = it->id;
		ev.time = sim::weapons::tick_end(p_message.server_tick);
		for (size_t k = 0; k < 3; ++k) {
			ev.pos[k] = it->position[k];
		}
		events_.push(ev);
		deployables_.erase(it);
	}
	for (const net::DeployableWire &d : p_message.added) {
		const auto it = std::find_if(
				deployables_.begin(), deployables_.end(), [&](const net::DeployableWire &o) { return o.id == d.id; });
		if (it != deployables_.end() || deployables_.size() >= kMaxPresentedDeployables) {
			++bad_deployables_;
			continue;
		}
		deployables_.push_back(d);
		PresentEvent ev;
		ev.kind = PresentKind::GadgetDeploy;
		ev.timeline = PresentTimeline::Immediate;
		ev.slot = d.owner;
		ev.aux = d.team;
		ev.def = d.def;
		ev.id = d.id;
		ev.time = sim::weapons::tick_end(p_message.server_tick);
		ev.flags = uint8_t(d.owner == own_slot_ ? present_flags::kOwnShooter : 0u);
		for (size_t k = 0; k < 3; ++k) {
			ev.pos[k] = d.position[k];
		}
		events_.push(ev);
	}
}

void ClientMatch::restrict(sim::Command &r_command) {
	if (!valid_) {
		return;
	}
	const sim::Command r = net::restrict_command(r_command, match_.restrict_until_tick, match_.restrict_mask);
	if (r.move_forward != r_command.move_forward || r.move_right != r_command.move_right || r.buttons != r_command.buttons) {
		++restricted_commands_;
	}
	r_command = r;
}

bool ClientMatch::unspawned(sim::Tick p_tick, bool p_have_own, sim::Tick p_life_spawn_tick) {
	unspawned_now_ = p_have_own && own_unspawned_at(valid_, match_, p_life_spawn_tick, p_tick);
	if (unspawned_now_) {
		++unspawned_ticks_;
	}
	return unspawned_now_;
}

void ClientMatch::publish(PresentationFrame &r_frame, bool p_have_own, uint32_t p_own_life) const {
	r_frame.match_valid = valid_;
	r_frame.match = match_;
	r_frame.player_count = player_rows(players_, r_frame.players);

	const bool dead_life = death_valid_ && p_have_own && p_own_life == death_life_;
	r_frame.own_respawn_tick = dead_life ? respawn_tick_ : 0u;
	r_frame.own_revive_until = dead_life ? revive_until_ : 0u;
	r_frame.own_unspawned = unspawned_now_;
	r_frame.gear = gear_;
	r_frame.squad_orders = orders_;
	r_frame.deployable_count = uint16_t(std::min(deployables_.size(), r_frame.deployables.size()));
	std::copy_n(deployables_.begin(), r_frame.deployable_count, r_frame.deployables.begin());
	r_frame.spots = spots_;
	r_frame.own_order = own_order_;
	r_frame.conquest_valid = conquest_valid_;
	r_frame.conquest = conquest_;
	r_frame.spawn_answer_serial = spawn_answers_;
	r_frame.spawn_answer = spawn_answer_;
}

void ClientMatch::stats(ClientStats &r_stats) const {
	r_stats.match_states = match_states_;
	r_stats.player_lists = player_lists_;
	r_stats.soldier_gears = soldier_gears_;
	r_stats.revives = revives_;
	r_stats.spot_state_messages = spot_state_messages_;
	r_stats.orders_received = own_order_.serial;
	r_stats.conquest_states = conquest_states_;
	r_stats.spawn_answers = spawn_answers_;
	r_stats.deployable_messages = deployable_messages_;
	r_stats.bad_deployables = bad_deployables_;
	r_stats.bad_tdm_datagrams = bad_tdm_datagrams_;
	r_stats.restricted_commands = restricted_commands_;
	r_stats.unspawned_ticks = unspawned_ticks_;
	r_stats.rules_mismatch = rules_mismatch_;
	logic_.stats(r_stats);
}

}
