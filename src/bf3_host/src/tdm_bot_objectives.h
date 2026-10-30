#pragma once

#include "bf3/host/server_view.h"
#include "bf3/sim/bots/perception.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "tdm_bot_movement.h"
#include "tdm_bot_shared.h"
#include "tdm_bot_targeting.h"
#include "tdm_bot_weapons.h"

#include <cstdint>
#include <span>

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
	bool defib_ready(sim::Tick p_tick) const { return p_tick >= defib_next_; }
	void fired_defib(sim::Tick p_tick);

private:
	const DownedSoldier *find(const ServerView &p_view) const;

	int slot_ = -1;
	sim::Tick since_ = 0, defib_next_ = 0;
};

class BotGrenade {
public:
	void new_life() { phase_ = 0; }
	bool active() const { return phase_ != 0; }
	bool throw_ready() const { return phase_ == 2; }

	bool can_start(const TdmShared &p_shared, const BotTargeting &p_targeting,
			const sim::bots::Perception &p_perception, sim::Tick p_tick, const sim::SoldierState &p_self,
			std::span<const sim::SoldierState> p_states, bool p_have, sim::Vec3 *r_at = nullptr) const;
	bool start(TdmShared &r_shared, BotTargeting &r_targeting, const sim::bots::Perception &p_perception, sim::Tick p_tick, const sim::SoldierState &p_self,
			std::span<const sim::SoldierState> p_states, bool p_have);
	void step(const BotArms &p_arms, sim::Tick p_tick, const sim::Vec3 &p_eye, uint8_t &r_slot, Trigger &r_trigger, float &r_yaw, float &r_pitch, bool &r_aim);

private:
	int phase_ = 0;
	sim::Tick phase_tick_ = 0, next_ = 0;
	uint32_t hold_ = 0;
	sim::Vec3 at_{};
};

class BotRetreat {
public:
	void new_life() { since_ = regen_since_ = clear_since_ = 0; }
	bool recovered(const TdmBotConfig &p_config, bool p_waiting, sim::Tick p_tick, float p_health) const;
	bool cleared(sim::Tick p_tick) const;
	bool running(sim::Tick p_tick) const;
	void begin_wait(sim::Tick p_tick) { regen_since_ = p_tick; }
	void seen(sim::Tick p_tick) { clear_since_ = p_tick; }
	const sim::Vec3 &threat() const { return threat_; }

	bool start(TdmShared &r_shared, BotNavigation &r_navigation, sim::Tick p_tick, const sim::SoldierState &p_self, const sim::Vec3 &p_threat, uint64_t &r_rng);

private:
	sim::Tick since_ = 0, regen_since_ = 0, clear_since_ = 0;
	sim::Vec3 threat_{};
};

class BotHunt {
public:
	void new_life() { searched_ = since_ = block_until_ = 0; }
	bool wanted(sim::Tick p_tick, const sim::bots::Perception &p_perception, const sim::SoldierState &p_self) const;
	bool enemy_within(const sim::bots::Perception &p_perception, const sim::SoldierState &p_self, float p_metres) const;
	void begin(sim::Tick p_tick) { since_ = p_tick; }
	void step(TdmShared &r_shared, BotNavigation &r_navigation, const sim::bots::Perception &p_perception, sim::Tick p_tick, const sim::SoldierState &p_self);

private:
	int known_enemy(const sim::bots::Perception &p_perception, const sim::SoldierState &p_self) const;

	sim::Tick searched_ = 0;
	sim::Tick since_ = 0, block_until_ = 0;
};

}
