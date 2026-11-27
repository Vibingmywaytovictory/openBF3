#pragma once

#include "bf3/ai/vehicle/car_control.h"
#include "bf3/sim/command.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace bf3::host {

struct TdmShared;

inline constexpr uint32_t kVehiclePathKey = 0x10000u;

class BotDriver {
public:
	void new_life();
	bool drive(TdmShared &r_shared, uint8_t p_slot, sim::Tick p_tick, const std::optional<sim::Vec3> &p_goal,
			float p_arrive_m, sim::Command &r_cmd);
	bool stuck() const { return stuck_; }

private:
	void request(TdmShared &r_shared, uint8_t p_slot, const sim::Vec3 &p_from, float p_radius, const sim::Vec3 &p_goal);
	void release(float p_forward_speed, sim::Command &r_cmd);

	std::optional<sim::Vec3> path_goal_;
	sim::NavCorridor corridor_;
	std::vector<sim::NavPolyRef> polys_;
	bool requested_ = false;
	bool following_ = false;
	bool stuck_ = false;
	int status_ = 0;
	ai::CarProgress progress_;
	ai::CarAngleDiff angles_;
};

}
