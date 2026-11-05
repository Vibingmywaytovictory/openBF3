#pragma once

#include "bf3/host/game_server.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <vector>

namespace bf3::host {

inline constexpr uint32_t kStuckWindowTicks = 1200;
inline constexpr float kStuckMetres = 1.0f;

class StuckBots {
public:
	void sample(const GameServerStats &p_game, uint64_t p_level);
	uint32_t count() const { return count_; }

private:
	struct Track {
		uint32_t life = 0;
		sim::Tick since = 0;
		sim::Vec3 at{};
		bool counted = false;
		bool alive = false;
	};

	std::vector<Track> tracks_;
	uint64_t level_ = 0;
	sim::Tick last_tick_ = 0;
	uint32_t count_ = 0;
};

}
