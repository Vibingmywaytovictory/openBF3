#include "bf3/core/ui/squad_order_marks.h"

namespace bf3::core::ui {

std::optional<HudSquadOrderLook> squad_order_look(HudSquadOrder p_order) {
	switch (p_order) {
		case HudSquadOrder::Attack:
			return HudSquadOrderLook{ kUiHudIconObjectiveAttack, "ObjectiveAttack", "UIHudIcon_ObjectiveAttack" };
		case HudSquadOrder::Defend:
			return HudSquadOrderLook{ kUiHudIconObjectiveDefend, "ObjectiveDefend", "UIHudIcon_ObjectiveDefend" };
		case HudSquadOrder::Move:
			return HudSquadOrderLook{ kUiHudIconObjectiveMoveTo, "ObjectiveMoveTo", "UIHudIcon_ObjectiveMoveTo" };
		case HudSquadOrder::None:
			break;
	}
	return std::nullopt;
}

std::optional<HudWorldTag> squad_order_tag(uint32_t p_id, HudSquadOrder p_order, const Icon3dPlacement &p_placement) {
	const std::optional<HudSquadOrderLook> look = squad_order_look(p_order);
	if (!look || !p_placement.shown) {
		return std::nullopt;
	}
	HudWorldTag tag;
	tag.id = p_id;
	tag.component = HudTagComponent::Mapmarker;
	tag.x = p_placement.x;
	tag.y = p_placement.y;
	tag.scale = p_placement.scale;
	tag.alpha = p_placement.alpha;
	tag.data.push_back(HudDataEntry{ "Icons", As2Value::of(look->marker_icon), true });
	tag.data.push_back(HudDataEntry{ "Snap", As2Value::of(p_placement.snapped), true });
	return tag;
}

}
