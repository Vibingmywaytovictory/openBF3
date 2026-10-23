#pragma once

#include "bf3/sim/bots/perception.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "tdm_bot_awareness.h"
#include "tdm_bot_shared.h"

#include <cstdint>
#include <span>

namespace bf3::host::tdm_bot {

bool visible_recent(const sim::bots::PerceivedEnemy *p_enemy, sim::Tick p_tick);

struct Sighting {
	const sim::bots::PerceivedEnemy *enemy = nullptr;
	bool visible = false;
};

struct AimError {
	float yaw = 0.0f;
	float pitch = 0.0f;
};

class BotTargeting {
public:
	void new_life();
	void drop() { target_ = -1; }

	void choose(TdmShared &r_shared, const BotAwareness &p_awareness, sim::Tick p_tick, const sim::SoldierState &p_self,
			std::span<const sim::SoldierState> p_states, float p_skill01, uint64_t &r_rng);
	Sighting track(TdmShared &r_shared, const sim::bots::Perception &p_perception, sim::Tick p_tick, std::span<const sim::SoldierState> p_states,
			uint64_t &r_rng);
	AimError aim_error(sim::Tick p_tick, float p_sigma, uint64_t &r_rng);

	int target() const { return target_; }
	sim::Tick target_since() const { return target_since_; }
	sim::Tick react_at() const { return react_at_; }
	sim::Tick last_engaged() const { return last_engaged_; }
	sim::Tick blind_until() const { return blind_until_; }
	const sim::Vec3 &blind_at() const { return blind_at_; }
	int lost_target() const { return lost_target_; }
	void forget_lost() { lost_target_ = -1; }

private:
	int target_ = -1;
	sim::Tick target_since_ = 0, react_at_ = 0, last_engaged_ = 0;
	float err_yaw_ = 0.0f, err_pitch_ = 0.0f;
	sim::Tick err_next_ = 0;
	int lost_target_ = -1;
	bool loss_handled_ = false;
	sim::Tick blind_until_ = 0;
	sim::Vec3 blind_at_{};
};

}
