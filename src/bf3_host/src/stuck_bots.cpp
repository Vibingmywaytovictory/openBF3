#include "bf3/host/stuck_bots.h"

#include <cmath>

namespace bf3::host {

void StuckBots::sample(const GameServerStats &p_game, uint64_t p_level) {
	if (p_level != level_ || p_game.tick < last_tick_) {
		tracks_.clear();
		level_ = p_level;
	}
	last_tick_ = p_game.tick;
	const net::MatchState &ms = p_game.match;
	const bool held = p_game.rules_active &&
			(ms.phase != net::MatchPhase::InRound ||
					(ms.restrict_until_tick != 0 && p_game.tick + 1u < ms.restrict_until_tick));
	tracks_.resize(p_game.slots.size());
	for (size_t i = 0; i < p_game.slots.size(); ++i) {
		const SlotStats &s = p_game.slots[i];
		Track &t = tracks_[i];
		if (held || !s.used || s.remote || !s.alive || s.life != t.life || !t.alive) {
			t = Track{};
			if (!held && s.used && !s.remote && s.alive) {
				t.alive = true;
				t.life = s.life;
				t.since = p_game.tick;
				t.at = s.state.position;
			}
			continue;
		}
		if (p_game.tick - t.since >= kStuckWindowTicks) {
			const float dx = s.state.position[0] - t.at[0], dz = s.state.position[2] - t.at[2];
			if (std::sqrt(dx * dx + dz * dz) < kStuckMetres && !t.counted) {
				++count_;
				t.counted = true;
			}
			t.since = p_game.tick;
			t.at = s.state.position;
		}
	}
}

}
