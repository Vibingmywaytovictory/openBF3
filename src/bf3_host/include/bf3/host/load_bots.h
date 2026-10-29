#pragma once

#include "bf3/host/game_server.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>

namespace bf3::host {

class LegWalker final : public CommandSource {
public:
	static constexpr uint32_t kLegTicks = 2u * sim::kSimHz;

	explicit LegWalker(uint32_t p_phase) : phase_(p_phase) {}
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override;

private:
	uint32_t phase_ = 0;
	bool started_ = false;
	uint16_t base_yaw_ = 0;
	sim::Tick start_ = 0;
};

}
