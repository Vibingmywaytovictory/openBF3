#pragma once

#include "bf3/sim/sim_types.h"

#include <cstdint>

namespace bf3::sim::bots {

struct AimParams {
	float e0_deg = 3.0f;
	float e_inf_deg = 0.5f;
	float tau_s = 0.6f;
	float moving_penalty_deg = 1.5f;
	float crossing_deg_per_mps = 0.5f;
	float skill_scale = 1.0f;
};

float aim_error(const AimParams &p_params, float p_skill, float p_seconds_on_target, bool p_moving, float p_crossing_speed_mps);

}
