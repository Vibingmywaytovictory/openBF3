// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBot/BotMemory.cpp @c47317b (modifyBehaviourUrgency)
#include "bf3/ai/bot/bot_memory.h"

#include "bf3/ai/behaviour/behaviours.h"

namespace bf3::ai {

float BotMemory::modify_behaviour_urgency(const BotBehaviour &p_behaviour, double p_time) const {
	if (int(p_behaviour.kind()) == last_behaviour) {
		return p_behaviour.urge_curve().calculate(float(p_time - chosen_time));
	}
	return 1.0f;
}

}
