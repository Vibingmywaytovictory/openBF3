#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"

#include <string_view>

namespace bf3::core::gameplay {

inline constexpr std::string_view kEndOfRoundLogicPartition = "ui/flow/logic/uiendofroundlogic";

struct EorTimeline {
	float fade_s = 0.0f;
	float scoreboard_delay_s = 0.0f;
	float request_eor_delay_s = 0.0f;
	float pre_eor_s = 0.0f;
	float eor_s = 0.0f;
	float restart_delay_s = 0.0f;
};

Result<EorTimeline> read_eor_timeline(const EbxResolver &p_db, WorkBudget *p_budget = nullptr);

}
