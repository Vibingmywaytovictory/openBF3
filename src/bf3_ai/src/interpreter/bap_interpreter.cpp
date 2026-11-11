#include "bf3/ai/interpreter/bap_interpreter.h"

#include "bf3/ai/interpreter/infantry_entries.h"

#include <algorithm>
#include <vector>

namespace bf3::ai {

namespace {

using T = BAPStatementType;
using D = AiDomain;

constexpr PlanEntryKind kKinds[] = {
	{ "EntryTankMoveTo", T::MoveTo, D::Vehicle },
	{ "EntryTankMoveToObject", T::MoveToObject, D::Vehicle },
	{ "EntryTankMoveToDirection", T::MoveToDirection, D::Vehicle },
	{ "EntryTankResetControls", T::ResetControls, D::Vehicle },
	{ "EntryTankTurnTo", T::Turn, D::Vehicle },
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

constexpr BAPStatementType kDefaultTypes[] = { T::Parallel, T::Serial, T::PlanEnd, T::If, T::Wait, T::While, T::Idle,
	T::InfoWrapper, T::FinishedWrapper, T::EvaluateCondition };

uint64_t type_bit(BAPStatementType p_type) {
	return uint64_t(1) << unsigned(p_type);
}

bool valid(BAPStatementType p_type) {
	return int(p_type) >= 0 && int(p_type) < kBAPStatementTypeCount;
}

struct InfantryInterpreter {
	std::vector<std::unique_ptr<BAPEntry>> entries;
	BAPInterpreter interpreter;

	InfantryInterpreter() {
		const AiSettings settings;
		for (const PlanEntryKind &kind : kKinds) {
			if (kind.domain != AiDomain::Infantry && kind.domain != AiDomain::Any) {
				continue;
			}
			std::unique_ptr<BAPEntry> entry = create_plan_entry(kind.name, settings);
			if (entry != nullptr && interpreter.add_entry(*entry)) {
				entries.push_back(std::move(entry));
			}
		}
		interpreter.activate_defaults();
	}
};

}

bool BAPInterpreter::execute(BAPStatement &r_statement, PlanBot &r_bot, bool &r_ok, bool &r_finished) const {
	if (r_bot.is_just_spawned()) {
		r_finished = false;
		r_ok = true;
		return true;
	}
	const BAPStatementType type = r_statement.id();
	const size_t lod = size_t(r_bot.lod_level());
	if (valid(type) && lod < kBotLodLevelCount && (active_[lod] & type_bit(type)) != 0) {
		const BAPEntry *const handler = entries_[size_t(type)][lod];
		return handler == nullptr ? r_statement.execute(r_bot, r_ok, r_finished)
								  : handler->execute(r_statement, r_bot, r_ok, r_finished);
	}
	r_finished = true;
	r_ok = false;
	return false;
}

bool BAPInterpreter::add_entry(const BAPEntry &p_entry, BotLodLevel p_lod) {
	const BAPStatementType type = p_entry.target_id();
	if (!valid(type) || entries_[size_t(type)][size_t(p_lod)] != nullptr) {
		return false;
	}
	for (size_t level = size_t(p_lod); level < kBotLodLevelCount; ++level) {
		if (entries_[size_t(type)][level] != nullptr) {
			break;
		}
		entries_[size_t(type)][level] = &p_entry;
		active_[level] |= type_bit(type);
	}
	return true;
}

bool BAPInterpreter::remove_entry(BotLodLevel p_lod, const BAPEntry &p_entry) {
	const BAPStatementType type = p_entry.target_id();
	if (!valid(type)) {
		return false;
	}
	for (size_t level = size_t(p_lod); level < kBotLodLevelCount; ++level) {
		if (entries_[size_t(type)][level] == &p_entry) {
			entries_[size_t(type)][level] = nullptr;
			active_[level] &= ~type_bit(type);
		}
	}
	return true;
}

bool BAPInterpreter::activate_entry(BAPStatementType p_type, BotLodLevel p_lod) {
	if (!valid(p_type) || entries_[size_t(p_type)][size_t(p_lod)] != nullptr) {
		return false;
	}
	for (size_t level = size_t(p_lod); level < kBotLodLevelCount; ++level) {
		if (entries_[size_t(p_type)][level] != nullptr) {
			break;
		}
		active_[level] |= type_bit(p_type);
	}
	return true;
}

void BAPInterpreter::activate_defaults() {
	for (const BAPStatementType type : kDefaultTypes) {
		activate_entry(type);
	}
}

bool BAPInterpreter::is_active(BAPStatementType p_type, BotLodLevel p_lod) const {
	return valid(p_type) && (active_[size_t(p_lod)] & type_bit(p_type)) != 0;
}

const BAPEntry *BAPInterpreter::entry(BAPStatementType p_type, BotLodLevel p_lod) const {
	return valid(p_type) ? entries_[size_t(p_type)][size_t(p_lod)] : nullptr;
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
	if (kind->name == "EntryInfanteryMoveTo") {
		return std::make_unique<EntryInfanteryMoveTo>();
	}
	if (kind->name == "EntryInfanteryMoveToObject") {
		return std::make_unique<EntryInfanteryMoveToObject>();
	}
	if (kind->name == "EntryInfanteryMoveToDirection") {
		return std::make_unique<EntryInfanteryMoveToDirection>();
	}
	if (kind->name == "EntryCarMoveToDirection") {
		return std::make_unique<EntryCarMoveToDirection>();
	}
	if (kind->name == "EntrySoldierPose") {
		return std::make_unique<EntrySoldierPose>();
	}
	if (kind->name == "EntryTrigger") {
		return std::make_unique<EntryTrigger>();
	}
	if (kind->name == "EntrySense") {
		return std::make_unique<EntrySense>();
	}
	return nullptr;
}

const BAPInterpreter &infantry_interpreter() {
	static const InfantryInterpreter s_infantry;
	return s_infantry.interpreter;
}

}
