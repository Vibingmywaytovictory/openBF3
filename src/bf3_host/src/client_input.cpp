#include "client_input.h"

namespace bf3::host {

void ClientInput::set(const InputSample &p_input) {
	std::lock_guard lock(mutex_);
	input_ = p_input;
}

InputSample ClientInput::latest() const {
	std::lock_guard lock(mutex_);
	return input_;
}

sim::Command ClientInput::sample(sim::Tick p_tick, uint32_t p_own_life, const sim::SoldierState &p_spawn_state) {
	const InputSample s = latest();
	sim::Command c;
	c.tick = p_tick;
	c.sequence = next_sequence_++;
	const bool jumped = jump_seen_ && s.jump_presses != last_jump_presses_;
	jump_seen_ = true;
	last_jump_presses_ = s.jump_presses;
	if (s.life < p_own_life) {
		c.yaw = p_spawn_state.yaw;
		c.pitch = p_spawn_state.pitch;
		c.pose = p_spawn_state.target_pose;
		keep_pose_ = p_spawn_state.target_pose;
		return c;
	}
	if (s.free_camera) {
		c.yaw = last_made_.yaw;
		c.pitch = last_made_.pitch;
		c.pose = keep_pose_;
		return c;
	}
	c.move_forward = sim::quantize_axis(s.move_forward);
	c.move_right = sim::quantize_axis(s.move_right);
	c.yaw = sim::quantize_yaw(s.yaw);
	c.pitch = sim::quantize_pitch(s.pitch);
	c.buttons = s.held_buttons & sim::buttons::kWireMask & ~sim::buttons::kEdgeMask;
	if (jumped) {
		c.buttons |= sim::buttons::kJump;
	}
	if (s.pose_request == PoseRequest::Set && uint8_t(s.pose) < sim::kPoseCount) {
		keep_pose_ = s.pose;
	}
	c.pose = keep_pose_;
	return c;
}

}
