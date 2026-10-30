#include "bf3/host/downed_mates.h"

namespace bf3::host {

void DownedMates::on_events(std::span<const present::PresentEvent> p_events) {
	for (const present::PresentEvent &e : p_events) {
		switch (e.kind) {
			case present::PresentKind::Death:
				if (e.aux < revive_until_.size()) {
					revive_until_[e.aux] = e.seed;
				}
				break;
			case present::PresentKind::Revive:
				if (e.aux < revive_until_.size()) {
					revive_until_[e.aux] = 0;
				}
				break;
			default:
				break;
		}
	}
}

void DownedMates::on_phase(net::MatchPhase p_phase) {
	if (p_phase != phase_) {
		phase_ = p_phase;
		clear();
	}
}

std::vector<uint8_t> DownedMates::downed(
		uint64_t p_now_tick, uint8_t p_own_team, uint8_t p_own_slot, std::span<const net::PlayerRow> p_players) const {
	std::vector<uint8_t> out;
	for (const net::PlayerRow &row : p_players) {
		if (row.slot >= revive_until_.size() || row.slot == p_own_slot || row.team != p_own_team || p_own_team == 0 ||
				row.alive || revive_until_[row.slot] <= p_now_tick) {
			continue;
		}
		out.push_back(row.slot);
	}
	return out;
}

}
