#include "bf3/sim/vehicles/vehicle_weapon_rules.h"

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

}
