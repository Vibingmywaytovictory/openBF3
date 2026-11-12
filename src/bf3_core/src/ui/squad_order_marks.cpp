#include "bf3/core/ui/squad_order_marks.h"

#include <cmath>
#include <string>

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

HudWorldTag capture_point_tag(uint32_t p_id, const HudCapturePoint &p_point, char p_letter, std::string_view p_name,
		const Icon3dPlacement &p_placement) {
	HudWorldTag tag;
	tag.id = p_id;
	tag.component = HudTagComponent::Capturepoint;
	tag.x = p_placement.x;
	tag.y = p_placement.y;
	tag.scale = p_placement.scale;
	tag.alpha = p_placement.alpha;
	const int32_t icon = p_point.hq
			? (p_point.owner == 1 ? kUiHudIconFriendlyBase : kUiHudIconEnemyBase)
			: (p_point.owner == 0 ? kUiHudIconNeutralFlag
								  : (p_point.owner == 1 ? kUiHudIconFriendlyFlag : kUiHudIconEnemyFlag));
	tag.data.push_back(HudDataEntry{ "Icons", As2Value::of(icon), true });
	tag.data.push_back(HudDataEntry{ "Team", As2Value::of(int32_t(p_point.owner)), true });
	tag.data.push_back(
			HudDataEntry{ "Label", As2Value::of(p_letter != 0 ? std::string(1, p_letter) : std::string()), true });
	tag.data.push_back(HudDataEntry{ "ProgressingTeam", As2Value::of(int32_t(p_point.attacker)), true });
	tag.data.push_back(HudDataEntry{ "Progress", As2Value::of(double(std::lround(p_point.progress * 100.0f))), true });
	tag.data.push_back(HudDataEntry{ "Blink", As2Value::of(p_point.contested), true });
	tag.data.push_back(HudDataEntry{ "Name", As2Value::of(std::string(p_name)), true });
	tag.data.push_back(HudDataEntry{ "Snap", As2Value::of(p_placement.snapped), true });
	return tag;
}
}
