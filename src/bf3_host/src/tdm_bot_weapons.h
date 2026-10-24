#pragma once

#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/weapon_input.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"
#include "tdm_bot_common.h"
#include "tdm_bot_shared.h"

#include <cstdint>

namespace bf3::host::tdm_bot {

enum class Trigger : uint8_t { None, Aimed, Blind, Defib, Grenade, Knife };

class BotArms {
public:
	BotArms(const Gunplay *p_gunplay, uint8_t p_slot);

	const sim::weapons::SoldierWeaponsState *weapons() const { return ws_; }
	const sim::weapons::WeaponSimDef *def(uint8_t p_slot) const;
	int32_t rounds(uint8_t p_slot) const;

private:
	const sim::weapons::SoldierWeaponsState *ws_ = nullptr;
	const sim::weapons::WeaponTable *table_ = nullptr;
};

struct FireRequest {
	sim::Tick tick = 0;
	sim::Tick life_tick = 0;
	uint8_t want_slot = kSlotPrimary;
	Trigger trigger = Trigger::None;
	bool aligned = false;
	bool sprint = false;
	bool zoom = false;
	bool quiet = false;
	bool throw_ready = false;
	bool defib_ready = false;
	sim::Tick react_at = 0;
	float target_distance = 0.0f;
	int target = -1;
	sim::Tick last_visible = 0;
	float skill = 0.0f;
	sim::Vec3 position{};
	sim::Vec3 eye{};
	float view_yaw = 0.0f, view_pitch = 0.0f;
};

struct WeaponCommand {
	sim::weapons::WeaponInput input;
	float comp_yaw = 0.0f, comp_pitch = 0.0f;
	bool defib_fired = false;
};

class BotWeapons {
public:
	void new_life(sim::Tick p_tick);
	void hold_fire() { bursting_ = false; }
	uint8_t preferred_slot() const { return sidearm_ ? kSlotSidearm : kSlotPrimary; }

	WeaponCommand input(TdmShared &r_shared, uint8_t p_slot, const BotArms &p_arms, const FireRequest &p_request, uint64_t &r_rng);

private:
	bool bursting_ = false;
	uint32_t burst_rounds_ = 0, burst_start_ = 0;
	sim::Tick pause_until_ = 0, next_tap_ = 0;
	bool tapped_ = false;
	uint8_t mode_counter_ = 0;
	sim::Tick mode_wait_ = 0;
	bool reload_held_ = false;
	bool sidearm_ = false;
};

bool friend_in_line(const TdmShared &p_shared, uint8_t p_slot, const sim::Vec3 &p_position, const sim::Vec3 &p_eye, float p_yaw, float p_pitch, float p_length);

}
