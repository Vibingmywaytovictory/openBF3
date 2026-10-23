#pragma once

#include "bf3/host/server_view.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "tdm_bot_movement.h"
#include "tdm_bot_shared.h"

#include <cstdint>

namespace bf3::host::tdm_bot {

struct ReviveCheck {
	sim::Tick tick = 0;
	uint8_t slot = 0xFF;
	uint8_t kit = 0;
	bool defibrillator = false;
	const sim::SoldierState *self = nullptr;
	sim::Vec3 eye{};
	bool shot_recent = false;
	sim::Tick shot_at_tick = 0;
	sim::Tick last_engaged = 0;
	const sim::SoldierState *seen = nullptr;
};

class BotRevive {
public:
	void new_life() { slot_ = -1; }
	const DownedSoldier *update(TdmShared &r_shared, BotNavigation &r_navigation, const ReviveCheck &p_check);
	sim::Tick &defib_next() { return defib_next_; }

private:
	const DownedSoldier *find(const ServerView &p_view) const;

	int slot_ = -1;
	sim::Tick since_ = 0, defib_next_ = 0;
};

}
