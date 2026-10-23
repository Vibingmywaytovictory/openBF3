#include "server_combat_log.h"

#include <optional>
#include <utility>
#include <vector>

namespace bf3::host {

void CombatLog::hit(const net::HitEvent &p_event) {
	events_.hits.push_back(p_event);
}

void CombatLog::death(const net::DeathEvent &p_event) {
	if (p_event.victim < slots_.size()) {
		ServerSlot &s = slots_[p_event.victim];
		if (s.used && !s.dummy && !s.want_spawn) {
			const std::optional<uint32_t> at = match_.policy().respawn_tick(p_event.victim, tick_);
			if (at) {
				s.want_spawn = true;
				s.respawn_at = *at;
			}
		}
	}
	net::DeathEvent e = p_event;
	TdmRules *tdm = match_.rules();
	if (tdm != nullptr) {
		e.respawn_tick = tdm->earliest_respawn(p_event.victim);
		e.revive_until = tdm->revive_until(p_event.victim);
		e.killer_health = combat_.killer_health(p_event.killer, p_event.victim);
	}
	events_.deaths.push_back(e);
	if (tdm != nullptr) {
		tdm->death(e);
	}
}

void CombatLog::revive(const net::ReviveEvent &p_event) {
	events_.revives.push_back(p_event);
	if (match_.rules() != nullptr) {
		match_.rules()->revive(p_event);
	}
}

void CombatLog::detonation(const net::DetonationWire &p_event) {
	events_.detonations.push_back(p_event);
}

void CombatLog::send(uint32_t p_peer, sim::Tick p_server_tick, Gunplay &r_gunplay, Outbox &r_outbox) const {
	if (events_.hits.empty() && events_.deaths.empty() && events_.detonations.empty() && events_.revives.empty()) {
		return;
	}
	net::CombatEvents c = events_;
	c.server_tick = p_server_tick;
	size_t clamped = 0;
	std::vector<uint8_t> bytes = net::encode_combat_events(c, combat_.frame(), &clamped);
	r_gunplay.count_combat_clamped(clamped);
	if (!bytes.empty()) {
		r_outbox.send(p_peer, net::kChannelReliable, std::move(bytes));
	}
}

}
