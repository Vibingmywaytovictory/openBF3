#pragma once

#include "bf3/sim/bots/perception.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "tdm_bot_shared.h"

#include <cstdint>
#include <span>

namespace bf3::host::tdm_bot {

class BotAwareness {
public:
	void configure(const sim::bots::PerceptionParams &p_params) { perception_ = sim::bots::Perception(p_params); }
	void new_life(sim::Tick p_tick);

	void check_shots(const TdmShared &p_shared, uint8_t p_slot, sim::Tick p_tick, const sim::SoldierState &p_self);
	void perceive(TdmShared &r_shared, uint8_t p_slot, uint64_t p_seed, sim::Tick p_tick, const sim::SoldierState &p_self,
			std::span<const sim::SoldierState> p_states, float p_yaw, float p_pitch);

	const sim::bots::Perception &perception() const { return perception_; }
	sim::Tick shot_at_tick() const { return shot_at_tick_; }
	int shot_by() const { return shot_by_; }
	const sim::Vec3 &shot_from() const { return shot_from_; }
	bool shot_recent(sim::Tick p_tick) const;

private:
	sim::bots::Perception perception_;
	sim::Tick shot_at_tick_ = 0;
	int shot_by_ = -1;
	sim::Vec3 shot_from_{};
	sim::Tick shots_checked_ = 0;
};

bool aims_at(const TdmShared &p_shared, const sim::SoldierState &p_other, const sim::SoldierState &p_self);

}
