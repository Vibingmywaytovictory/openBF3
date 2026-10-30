#pragma once

#include "bf3/net/protocol.h"
#include "bf3/present/presentation_events.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::host {

class DownedMates {
public:
	void on_events(std::span<const present::PresentEvent> p_events);
	void clear() { revive_until_.fill(0); }
	void on_phase(net::MatchPhase p_phase);
	std::vector<uint8_t> downed(uint64_t p_now_tick, uint8_t p_own_team, uint8_t p_own_slot,
			std::span<const net::PlayerRow> p_players) const;
	uint64_t revive_until(uint8_t p_slot) const { return p_slot < revive_until_.size() ? revive_until_[p_slot] : 0; }

private:
	std::array<uint64_t, net::kMaxSlots> revive_until_{};
	net::MatchPhase phase_ = net::MatchPhase::Loading;
};

}
