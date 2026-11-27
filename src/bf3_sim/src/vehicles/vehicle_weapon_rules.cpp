#include "bf3/sim/vehicles/vehicle_weapon_rules.h"

#include <algorithm>

namespace bf3::sim::vehicles {

void recoil_impulse_begin(RecoilImpulseState &r_state) {
	if (r_state.counter > 0) {
		--r_state.counter;
	}
}

void recoil_impulse_shot(RecoilImpulseState &r_state, uint8_t p_muzzles) {
	r_state.counter = kRecoilImpulseCounter;
	if (p_muzzles > 0) {
		r_state.muzzle = uint8_t((r_state.muzzle + 1u) % p_muzzles);
	}
}

bool recoil_impulse_due(const RecoilImpulseState &p_state, float p_impulse_strength) {
	return p_state.counter == 1 && p_impulse_strength > 0.0f;
}

// SOURCE: notes R-A3 H and R2-G2 B1 (the penalty runs down first, then heat falls and clears below OverHeatThreshold).
void overheat_tick(OverheatState &r_state, const OverheatRule &p_rule, float p_dt) {
	if (r_state.penalty > 0.0f) {
		r_state.penalty = std::max(0.0f, r_state.penalty - p_dt);
		return;
	}
	r_state.heat = std::max(0.0f, r_state.heat - p_rule.drop_per_second * p_dt);
	if (r_state.heat < p_rule.threshold) {
		r_state.overheated = false;
	}
}

bool overheat_shot(OverheatState &r_state, const OverheatRule &p_rule) {
	r_state.heat += p_rule.heat_per_bullet;
	if (r_state.heat < 1.0f) {
		return false;
	}
	r_state.heat = 1.0f;
	r_state.overheated = true;
	r_state.penalty = p_rule.penalty_time;
	return true;
}

}
