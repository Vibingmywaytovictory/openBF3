#pragma once

#include "bf3/host/client_session.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <mutex>

namespace bf3::host {

class ClientInput {
public:
	void set(const InputSample &p_input);
	InputSample latest() const;

	uint32_t next_sequence() const { return next_sequence_; }
	void keep_pose(sim::Pose p_pose) { keep_pose_ = p_pose; }
	sim::Command sample(sim::Tick p_tick, uint32_t p_own_life, const sim::SoldierState &p_spawn_state);
	void stepped(const sim::Command &p_command) { last_made_ = p_command; }

private:
	mutable std::mutex mutex_;
	InputSample input_;
	uint32_t next_sequence_ = 1;
	sim::Command last_made_;
	bool jump_seen_ = false;
	uint32_t last_jump_presses_ = 0;
	sim::Pose keep_pose_ = sim::Pose::Stand;
};

}
