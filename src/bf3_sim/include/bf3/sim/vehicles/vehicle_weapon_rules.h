#pragma once

#include <cstdint>

namespace bf3::sim::vehicles {

// SOURCE: notes R2-G2 D (counter 2 at the shot, the impulse in the update where it is 1; muzzle index mod n).
inline constexpr uint8_t kRecoilImpulseCounter = 2;

struct RecoilImpulseState {
	uint8_t counter = 0;
	uint8_t muzzle = 0;

	bool operator==(const RecoilImpulseState &) const = default;
};

void recoil_impulse_begin(RecoilImpulseState &r_state);

void recoil_impulse_shot(RecoilImpulseState &r_state, uint8_t p_muzzles);

bool recoil_impulse_due(const RecoilImpulseState &p_state, float p_impulse_strength);

// SOURCE: notes R2-G2 B1 (ReloadTimeMultiplier and OverheatDropPerSecondMultiplier on R-A3's reload and heat drop).
constexpr float vehicle_reload_seconds(float p_reload_time, float p_reload_time_multiplier) {
	return p_reload_time * p_reload_time_multiplier;
}

constexpr float vehicle_heat_drop_per_second(float p_heat_drop_per_second, float p_overheat_drop_multiplier) {
	return p_heat_drop_per_second * p_overheat_drop_multiplier;
}

struct OverheatRule {
	float heat_per_bullet = 0;
	float drop_per_second = 0;
	float penalty_time = 0;
	float threshold = 0;
};

struct OverheatState {
	float heat = 0;
	float penalty = 0;
	bool overheated = false;

	bool operator==(const OverheatState &) const = default;
};

void overheat_tick(OverheatState &r_state, const OverheatRule &p_rule, float p_dt);

bool overheat_shot(OverheatState &r_state, const OverheatRule &p_rule);

}
