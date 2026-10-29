#include "bf3/host/load_bots.h"

namespace bf3::host {

sim::Command LegWalker::command(sim::Tick p_tick, const sim::SoldierState &p_state) {
	if (!started_) {
		started_ = true;
		base_yaw_ = p_state.yaw;
		start_ = p_tick;
	}
	sim::Command c;
	c.tick = p_tick;
	c.move_forward = sim::kAxisMax;
	const uint32_t leg = ((p_tick - start_) / kLegTicks + phase_) & 3u;
	c.yaw = uint16_t(base_yaw_ + leg * 16384u);
	c.pose = sim::Pose::Stand;
	return c;
}

}
