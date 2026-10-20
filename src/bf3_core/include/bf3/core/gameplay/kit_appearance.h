#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace bf3::core::gameplay {

inline constexpr size_t kMaxKitVisualLinks = 4096;

struct KitAppearance {
	std::string kit;
	std::string visual_unlock;
	Guid visual_unlock_guid{};
	std::string appearance;
	size_t candidates = 0;
};

bool appearance_matches_kit(std::string_view p_kit_asset, std::string_view p_appearance);

Result<KitAppearance> read_kit_appearance(const EbxResolver &p_db, std::string_view p_kit_asset, WorkBudget *p_budget = nullptr);

}
