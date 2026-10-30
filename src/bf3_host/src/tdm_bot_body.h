#pragma once

#include "bf3/host/server_view.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "tdm_bot_awareness.h"
#include "tdm_bot_common.h"
#include "tdm_bot_movement.h"
#include "tdm_bot_objectives.h"
#include "tdm_bot_shared.h"
#include "tdm_bot_targeting.h"
#include "tdm_bot_weapons.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <span>

namespace bf3::host::tdm_bot {

struct BotFrame {
	sim::Tick tick;
	const sim::SoldierState &self;
	std::span<const sim::SoldierState> states;
	const sim::CollisionWorld &world;
	sim::Vec3 eye;
	const sim::bots::PerceivedEnemy *te;
	bool visible;
	int target;
	bool shot_recent;
	BotArms arms;
	const DownedSoldier *revive;
};

struct BotIntent {
	float move_dx = 0.0f, move_dz = 0.0f;
	bool sprint = false;
	sim::Pose pose = sim::Pose::Stand;
	float want_yaw = 0.0f, want_pitch = 0.0f;
	bool have_aim = false;
	bool zoom = false;
	float target_distance = 0.0f;
	uint8_t want_slot = kSlotPrimary;
	Trigger trigger = Trigger::None;
	float walk_dx = 0.0f, walk_dz = 0.0f, walk_yaw = 0.0f;
	bool walk_sprint = false;
	bool walk_jump = false;
	sim::Pose walk_pose = sim::Pose::Stand;

	float aim_at(const sim::Vec3 &p_eye, const sim::Vec3 &p_point) {
		const float dx = p_point[0] - p_eye[0], dy = p_point[1] - p_eye[1], dz = p_point[2] - p_eye[2];
		const float h = std::sqrt(dx * dx + dz * dz);
		want_yaw = yaw_of(dx, dz);
		want_pitch = std::atan2(dy, std::max(h, 1e-3f));
		have_aim = true;
		return std::sqrt(h * h + dy * dy);
	}
};

struct BotBody {
	std::shared_ptr<TdmShared> shared;
	uint32_t index = 0;
	uint8_t slot = 0xFF;
	uint64_t seed = 0;
	uint64_t rng = 0;
	BotAwareness awareness;
	BotNavigation navigation;

	uint32_t life = UINT32_MAX;
	sim::Tick life_tick = 0;
	uint8_t kit = 0;
	float skill = 0.5f, skill01 = 0.5f;
	float view_yaw = 0.0f, view_pitch = 0.0f;
	TdmBotActivity activity = TdmBotActivity::Dead;

	BotTargeting targeting;
	BotHunt hunt;
	BotWeapons weapons;
	BotStrafe strafe;
	BotLook look;
	BotRetreat retreat;
	BotRevive medic;
	BotGrenade grenade;

	sim::Tick knife_until = 0;

	const TdmBotConfig &cfg() const { return *shared->config; }

	void attach(std::shared_ptr<TdmShared> p_shared, uint32_t p_index);
	void set_slot(uint8_t p_slot);
	void new_life(sim::Tick p_tick, const sim::SoldierState &p_state);
	void set_activity(sim::Tick p_tick, TdmBotActivity p_activity);
	void restricted(sim::Tick p_tick, sim::Command &r_cmd);

	Sighting see(sim::Tick p_tick, const sim::SoldierState &p_state, std::span<const sim::SoldierState> p_states);
	BotFrame sense_revive(
			sim::Tick p_tick, const sim::SoldierState &p_state, const ServerView &p_view, const Sighting &p_sighting);
	BotFrame sense(sim::Tick p_tick, const sim::SoldierState &p_state, const ServerView &p_view);

	BotIntent begin_plan(TdmBotActivity p_activity);
	void face_shots(const BotFrame &p_frame, TdmBotActivity p_activity, BotIntent &r_intent);
	bool turn(const BotFrame &p_frame, BotIntent &r_intent);
	void compose(const BotFrame &p_frame, TdmBotActivity p_activity, const BotIntent &p_intent, bool p_aligned,
			sim::Command &r_cmd);

	bool walk(const BotFrame &p_frame, BotIntent &r_intent);
	void walk_facing(const BotFrame &p_frame, BotIntent &r_intent);
	void roam(const BotFrame &p_frame, BotIntent &r_intent);
	void hunt_step(const BotFrame &p_frame, BotIntent &r_intent);
	void wait_in_cover(const BotFrame &p_frame, BotIntent &r_intent);
	void crouch(BotIntent &r_intent);
	void face_threat(const BotFrame &p_frame, BotIntent &r_intent);

	void fight(const BotFrame &p_frame, BotIntent &r_intent);
	void aim_blind(const BotFrame &p_frame, BotIntent &r_intent);
	void trigger_blind(BotIntent &r_intent);
	void aim_target(const BotFrame &p_frame, BotIntent &r_intent);
	void trigger_target(const BotFrame &p_frame, BotIntent &r_intent);
	void move_fight(const BotFrame &p_frame, BotIntent &r_intent);

	void approach_revive(const BotFrame &p_frame, BotIntent &r_intent);
	bool revive_approach(const BotFrame &p_frame, BotIntent &r_intent);
	void revive_pose(const BotFrame &p_frame, BotIntent &r_intent);
	void revive_trigger(const BotFrame &p_frame, BotIntent &r_intent);

	void throw_step(const BotFrame &p_frame, BotIntent &r_intent);
};

}
