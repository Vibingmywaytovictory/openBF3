#include "bf3/ai/behaviour/vehicle_gadgets.h"

#include "bf3/ai/behaviour/curves.h"

namespace bf3::ai {

bool needs_assist(float p_health, bool p_human) {
	const float threshold = p_human ? kAssistHumanHealth : kAssistHealth;
	return threshold > p_health && p_health > 0.0f;
}

float assist_score(float p_strength, float p_health, float p_distance) {
	return p_strength / (DecliningSlopeCurve::calculate(1.0f - p_health) * p_distance);
}

float assist_reach(float p_replenish_radius, float p_radii) {
	return p_replenish_radius * 0.5f + p_radii;
}

ExplosivePhase placed_explosive_next(ExplosivePhase p_phase, const PlacedExplosiveCheck &p_check) {
	const PlacedExplosiveCheck &c = p_check;
	switch (p_phase) {
		case ExplosivePhase::Approach:
			return c.gap <= c.reach && c.loaded ? ExplosivePhase::Place : p_phase;
		case ExplosivePhase::Place:
			return c.thrown ? ExplosivePhase::Wait : p_phase;
		case ExplosivePhase::Wait:
			return !c.loaded || c.ready ? ExplosivePhase::BackOff : p_phase;
		case ExplosivePhase::BackOff:
			return c.gap >= kPlacedClearMetres ? ExplosivePhase::Detonate : p_phase;
		case ExplosivePhase::Detonate:
			return c.charges_out ? p_phase : ExplosivePhase::Approach;
	}
	return p_phase;
}

}
