#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <cstdint>

namespace bf3::ai {

class IAIPlayerCourier {
public:
	virtual ~IAIPlayerCourier() = default;
	virtual AiObject *requested_vehicle(int p_player_id) = 0;
	virtual AiObject *first_ability_target(int p_player_id) = 0;
	virtual AiObject *resurrect_target(int p_player_id) = 0;
	virtual uint32_t second_ability_target_cell(int p_player_id) = 0;
};

}
