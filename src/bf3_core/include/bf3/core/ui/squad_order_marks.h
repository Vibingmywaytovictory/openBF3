#pragma once

#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/icon3d_placement.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace bf3::core::ui {

enum class HudSquadOrder : uint8_t {
	None,
	Move,
	Attack,
	Defend,
};

inline constexpr int32_t kUiHudIconObjectiveDefend = 16;
inline constexpr int32_t kUiHudIconObjectiveMoveTo = 17;
inline constexpr int32_t kUiHudIconObjectiveAttack = 18;

struct HudSquadOrderLook {
	int32_t marker_icon = 0;
	std::string_view marker_frame;
	std::string_view minimap_icon;
};

std::optional<HudSquadOrderLook> squad_order_look(HudSquadOrder p_order);

std::optional<HudWorldTag> squad_order_tag(uint32_t p_id, HudSquadOrder p_order, const Icon3dPlacement &p_placement);

HudWorldTag capture_point_tag(
		uint32_t p_id, const HudCapturePoint &p_point, char p_letter, const Icon3dPlacement &p_placement);
}
