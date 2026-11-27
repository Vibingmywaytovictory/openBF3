// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBotBehaviour/BBMedicAssist.h, AIBotBehaviourPattern/BBPMedicAssist.h and
// BBPFireInfantery.h @c47317b
#pragma once

#include <cstdint>

namespace bf3::ai {

// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBotBehaviour/BBMedicAssist.cpp @c47317b (75 m, 0.75, 0.99, x2, x10) and
// AIBotBehaviourPattern/BBPFireInfantery.cpp (back off to 30 m, 9 m clear, 0.7 s) for the 8 constants below.
inline constexpr float kAssistRadiusMetres = 75.0f;
inline constexpr float kAssistHealth = 0.75f;
inline constexpr float kAssistHumanHealth = 0.99f;
inline constexpr float kAssistUrgency = 2.0f;
inline constexpr float kAssistHumanUrgency = 10.0f;
inline constexpr float kPlacedBackOffMetres = 30.0f;
inline constexpr float kPlacedClearMetres = 9.0f;
inline constexpr float kPlacedDetonateDelayS = 0.7f;

// STAND-IN: BBMedicAssist cut to its health test, score and curve; its call boosts (x1.5, x3), 100 urgency for a human
// caller within 50 m and 0.7071 ground test are left out. Settle: follow OpenBF2 BBMedicAssist.cpp @c47317b.
bool needs_assist(float p_health, bool p_human);
float assist_score(float p_strength, float p_health, float p_distance);
float assist_urgency(float p_score, bool p_human);
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

// STAND-IN: BBPFireInfantery's placed-explosive plan becomes one phase step the host body calls. Settle: port the plan's
// statements once the BAP actions drive a bot.
ExplosivePhase placed_explosive_next(ExplosivePhase p_phase, const PlacedExplosiveCheck &p_check);

}
