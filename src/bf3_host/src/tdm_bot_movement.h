#pragma once

#include "bf3/sim/bots/walker_brain.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"
#include "tdm_bot_shared.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace bf3::host::tdm_bot {

inline constexpr float kHuntArriveMetres = 3.0f;

class BotNavigation {
public:
	void set_slot(uint8_t p_slot, uint64_t p_seed);
	void new_life();

	bool set_goal(TdmShared &r_shared, sim::Tick p_tick, const sim::Vec3 &p_at, float p_arrive, bool p_force);
	void clear_goal();
	void pick_roam_goal(TdmShared &r_shared, sim::Tick p_tick, const sim::Vec3 &p_at, uint64_t &r_rng);

	bool walk(TdmShared &r_shared, sim::Tick p_tick, const sim::SoldierState &p_state, float &r_dx, float &r_dz, float &r_yaw, bool &r_sprint, sim::Pose &r_pose,
			bool &r_jump);

private:
	void check_island(TdmShared &r_shared, sim::Tick p_tick, const sim::SoldierState &p_state);
	std::optional<sim::Vec3> heat_goal(TdmShared &r_shared, sim::Tick p_tick, const sim::Vec3 &p_at, uint64_t &r_rng);

	uint8_t slot_ = 0xFF;
	uint64_t seed_ = 0;
	std::optional<sim::bots::WalkerBrain> walker_;
	std::vector<sim::bots::BotGoal> goals_;
	uint32_t walker_resets_ = 0;
	bool have_goal_ = false;
	sim::Vec3 goal_pos_{};
	sim::Tick goal_tick_ = 0;
	sim::Tick roam_tick_ = 0;

	bool off_main_ = false;
	sim::Vec3 exit_at_{};
	sim::Vec3 exit_from_{};
	sim::Tick exit_progress_tick_ = 0;
};

class BotStrafe {
public:
	void stop() { strafing_ = false; }
	void step(const TdmBotConfig &p_config, uint8_t p_slot, sim::Tick p_tick, const sim::CollisionWorld &p_world, const sim::SoldierState &p_self, float p_view_yaw,
			uint64_t &r_rng, float &r_dx, float &r_dz);

private:
	bool strafing_ = false;
	float dir_ = 1.0f, half_ = 1.0f;
	sim::Vec3 anchor_{};
	float ax_ = 1.0f, az_ = 0.0f;
	sim::Tick until_ = 0;
};

}
