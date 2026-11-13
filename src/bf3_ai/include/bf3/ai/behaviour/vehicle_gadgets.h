#pragma once

#include <cstdint>

namespace bf3::ai {

inline constexpr float kAssistRadiusMetres = 75.0f;
inline constexpr float kAssistHealth = 0.75f;
inline constexpr float kAssistHumanHealth = 0.99f;
inline constexpr float kPlacedBackOffMetres = 30.0f;
inline constexpr float kPlacedClearMetres = 9.0f;
inline constexpr float kPlacedDetonateDelayS = 0.7f;

bool needs_assist(float p_health, bool p_human);
float assist_score(float p_strength, float p_health, float p_distance);
float assist_reach(float p_replenish_radius, float p_radii);

enum class ExplosivePhase : uint8_t { Approach, Place, Wait, BackOff, Detonate };

struct PlacedExplosiveCheck {
	float gap = 0.0f;
	float reach = 0.0f;
	bool loaded = false;
	bool thrown = false;
	bool ready = false;
	bool charges_out = false;
};

ExplosivePhase placed_explosive_next(ExplosivePhase p_phase, const PlacedExplosiveCheck &p_check);

}
