#include "client_match.h"

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
}

void ClientMatch::stats(ClientStats &r_stats) const {
	r_stats.match_states = match_states_;
	r_stats.player_lists = player_lists_;
	r_stats.revives = revives_;
	r_stats.bad_tdm_datagrams = bad_tdm_datagrams_;
	r_stats.restricted_commands = restricted_commands_;
	r_stats.unspawned_ticks = unspawned_ticks_;
	r_stats.rules_mismatch = rules_mismatch_;
}

}
