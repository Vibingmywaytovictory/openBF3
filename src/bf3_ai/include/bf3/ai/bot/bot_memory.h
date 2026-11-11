#pragma once

namespace bf3::ai {

class BotBehaviour;

class BotMemory {
public:
	float modify_behaviour_urgency(const BotBehaviour &p_behaviour, double p_time) const;

	int last_behaviour = -1;
	double chosen_time = 0.0;
};

}
