#include "bf3/ai/plan/plan_entries.h"

#include "bf3/ai/plan/infantry_entries.h"

#include <algorithm>

namespace bf3::ai {

namespace {

using T = PlanTarget;
using D = AiDomain;

constexpr PlanEntryKind kKinds[] = {
	{ "EntryTankMoveTo", T::MoveTo, D::Vehicle },
	{ "EntryTankMoveToObject", T::MoveToObject, D::Vehicle },
	{ "EntryTankMoveToDirection", T::MoveToDirection, D::Vehicle },
	{ "EntryTankResetControls", T::ResetControls, D::Vehicle },
	{ "EntryTankTurnTo", T::TurnTo, D::Vehicle },
	{ "EntryCarMoveTo", T::MoveTo, D::Vehicle },
	{ "EntryCarMoveToDirection", T::MoveToDirection, D::Vehicle, true },
	{ "EntryCarMoveToObject", T::MoveToObject, D::Vehicle },
	{ "EntryCarResetControls", T::ResetControls, D::Vehicle },
	{ "EntryPlaneMoveTo", T::MoveTo, D::Air },
	{ "EntryPlaneMoveToDirection", T::MoveToDirection, D::Air },
	{ "EntryPlaneMoveToObject", T::MoveToObject, D::Air },
	{ "EntryPlaneAimAt", T::AimAt, D::Air },
	{ "EntryPlaneResetControls", T::ResetControls, D::Air },
	{ "EntryBoatMoveTo", T::MoveTo, D::Vehicle },
	{ "EntryBoatMoveToObject", T::MoveToObject, D::Vehicle },
	{ "EntryBoatMoveToDirection", T::MoveToDirection, D::Vehicle },
	{ "EntryBoatResetControls", T::ResetControls, D::Vehicle },
	{ "EntryInfanteryMoveTo", T::MoveTo, D::Infantry, true },
	{ "EntryInfanteryMoveToObject", T::MoveToObject, D::Infantry, true },
	{ "EntryInfanteryMoveToDirection", T::MoveToDirection, D::Infantry, true },
	{ "EntryInfanteryResetControls", T::ResetControls, D::Infantry },
	{ "EntryTrigger", T::Trigger, D::Any, true },
	{ "EntryTriggerContinously", T::TriggerContinously, D::Any },
	{ "EntryKeyTurretAimAt", T::AimAt, D::Vehicle },
	{ "EntryMouseTurretAimAt", T::AimAt, D::Vehicle },
	{ "EntryMouseTurretLookAt", T::LookAt, D::Vehicle },
	{ "EntrySense", T::Sense, D::Any, true },
	{ "EntrySoldierPose", T::SoldierPose, D::Infantry, true },
	{ "EntryMoveToMedium", T::MoveTo, D::Any },
	{ "EntryMoveToMediumBoat", T::MoveTo, D::Vehicle },
	{ "EntryMoveToMediumSoldier", T::MoveTo, D::Infantry },
	{ "EntryMoveToObjectMedium", T::MoveToObject, D::Any },
	{ "EntryMoveToObjectMediumBoat", T::MoveToObject, D::Vehicle },
	{ "EntryMoveToObjectMediumSoldier", T::MoveToObject, D::Infantry },
	{ "EntryInfoWrapper", T::InfoWrapper, D::Any },
	{ "EntryHeliMoveTo", T::MoveTo, D::Air },
	{ "EntryHeliMoveToDirection", T::MoveToDirection, D::Air },
	{ "EntryHeliMoveToObject", T::MoveToObject, D::Air },
	{ "EntryHeliAimAt", T::AimAt, D::Air },
	{ "EntryHeliResetControls", T::ResetControls, D::Air },
	{ "EntryHeliMoveAwayFromObject", T::MoveAwayFromObject, D::Air },
};

}

bool EntryInfanteryMoveToDirection::execute(BotScript *, Bot *, bool &, bool &) {
	return true;
}

bool EntryCarMoveToDirection::execute(BotScript *, Bot *, bool &, bool &) {
	return true;
}

std::span<const PlanEntryKind> plan_entry_kinds() {
	return kKinds;
}

const PlanEntryKind *find_plan_entry_kind(std::string_view p_name) {
	const auto found = std::find_if(
			std::begin(kKinds), std::end(kKinds), [&](const PlanEntryKind &p_kind) { return p_kind.name == p_name; });
	return found != std::end(kKinds) ? &*found : nullptr;
}

std::unique_ptr<BAPEntry> create_plan_entry(std::string_view p_name, const AiSettings &p_settings) {
	const PlanEntryKind *const kind = find_plan_entry_kind(p_name);
	if (kind == nullptr || !kind->has_body || !p_settings.domain_enabled(kind->domain)) {
		return nullptr;
	}
	if (kind->name == "EntryInfanteryMoveToDirection") {
		return std::make_unique<EntryInfanteryMoveToDirection>();
	}
	if (kind->name == "EntryCarMoveToDirection") {
		return std::make_unique<EntryCarMoveToDirection>();
	}
	return create_infantry_entry(kind->name);
}

}
