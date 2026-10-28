#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/result.h"

#include <array>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr std::string_view kEorFadingPartition = "gameplay/level_setups/components/mp_eor_fading";
inline constexpr std::string_view kEorFadingStartEvent = "StartFade";

struct EorFadingKey {
	float seconds = 0.0f;
	float value = 0.0f;
};

struct EorFading {
	float flash_s = 0.0f;
	std::vector<EorFadingKey> visibility;
	float ui_fade_s = 0.0f;
	float screen_fade_s = 0.0f;
	std::array<float, 3> brightness{ 1.0f, 1.0f, 1.0f };
	std::array<float, 3> contrast{ 1.0f, 1.0f, 1.0f };
	std::array<float, 3> saturation{ 1.0f, 1.0f, 1.0f };

	float visibility_at(float p_seconds) const;
	float screen_black_at(float p_seconds) const;
	float ui_alpha_at(float p_seconds) const;
};

Result<EorFading> read_eor_fading(const EbxResolver &p_db, level::WorkBudget &r_budget);

}
