#include "bf3/ai/main/behaviour_manager.h"

#include "bf3/ai/plan/plan_entries.h"
#include "bf3/core/util/text.h"

#include <algorithm>

namespace bf3::ai {

namespace {

using D = AiDomain;

constexpr BehaviourKind kKinds[] = {
	{ "BBAvoid", D::Any },
	{ "BBMoveTo", D::Any },
	{ "BBMoveToFixed", D::Any },
	{ "BBMedicAssist", D::Infantry },
	{ "BBIdle", D::Any },
	{ "BBFire", D::Vehicle },
	{ "BBFire3d", D::Air },
	{ "BBFireInfantery", D::Infantry },
	{ "BBFireUnarmed", D::Infantry },
	{ "BBFireLargeBore", D::Vehicle },
	{ "BBTakeCover", D::Vehicle },
	{ "BBTakeCoverInfantry", D::Infantry },
	{ "BBFireDriver", D::Vehicle },
	{ "BBChange", D::Vehicle },
	{ "BBChangeLandingCraft", D::Vehicle },
	{ "BBChangeTeleport", D::Vehicle },
	{ "BBSuicide", D::Any },
	{ "BBCallForMedic", D::Infantry },
	{ "BBRevive", D::Infantry },
	{ "BBTakeCover3d", D::Air },
	{ "BBC4", D::Infantry },
	{ "BBRunOverSoldier", D::Vehicle },
	{ "BBBridgeRepair", D::Infantry },
	{ "BBDefuseMines", D::Infantry },
	{ "BBRandom", D::Any },
	{ "BBTriggerable", D::Any },
};

}

std::span<const BehaviourKind> behaviour_kinds() {
	return kKinds;
}

bool BotBehaviourManager::NoCase::operator()(std::string_view p_a, std::string_view p_b) const {
	return std::lexicographical_compare(p_a.begin(), p_a.end(), p_b.begin(), p_b.end(),
			[](char p_x, char p_y) { return core::to_lower_ascii(p_x) < core::to_lower_ascii(p_y); });
}

BotBehaviourManager::BotBehaviourManager(const AiSettings &p_settings) : settings_(p_settings) {
	for (const BehaviourKind &kind : kKinds) {
		create_functions_[std::string(kind.name)] = Registration{ kind.domain, nullptr };
	}
}

bool BotBehaviourManager::reset() {
	behaviour_ids_.clear();
	urge_curves_.clear();
	behaviour_names_.clear();
	return true;
}

void BotBehaviourManager::set_behaviour_name(int p_id, const std::string &p_name) {
	behaviour_names_.resize(size_t(p_id) + 1);
	behaviour_names_[size_t(p_id)] = p_name;
	behaviour_ids_[p_name] = p_id;
}

int BotBehaviourManager::behaviour_id(std::string_view p_name) const {
	const auto found = behaviour_ids_.find(p_name);
	return found != behaviour_ids_.end() ? found->second : -1;
}

std::string BotBehaviourManager::behaviour_name(int p_id) const {
	if (p_id >= 0 && size_t(p_id) < behaviour_names_.size()) {
		return behaviour_names_[size_t(p_id)];
	}
	return "";
}

bool BotBehaviourManager::set_create_function(std::string_view p_kind, BBCreateFunction *p_function) {
	const auto found = create_functions_.find(p_kind);
	if (found == create_functions_.end()) {
		return false;
	}
	found->second.function = p_function;
	return true;
}

bool BotBehaviourManager::is_registered(std::string_view p_kind) const {
	return create_functions_.find(p_kind) != create_functions_.end();
}

bool BotBehaviourManager::is_enabled(std::string_view p_kind) const {
	const auto found = create_functions_.find(p_kind);
	return found != create_functions_.end() && settings_.domain_enabled(found->second.domain);
}

int BotBehaviourManager::create(const std::string &p_behaviour_name, const std::string &p_create_function_name,
		const std::string &p_name, int p_mode, const std::string &p_urge_curve_name, const float *p_weights,
		std::string &r_message) {
	const int id = behaviour_id(p_behaviour_name);
	if (id == -1) {
		r_message = "'" + p_behaviour_name + "' doesn't exist";
		return 0;
	}
	const auto function = create_functions_.find(p_create_function_name);
	if (function == create_functions_.end()) {
		r_message = "'" + p_create_function_name + "' is not a behaviour kind";
		return 0;
	}
	if (!settings_.domain_enabled(function->second.domain)) {
		r_message = "'" + p_create_function_name + "' is off (ai.enable_vehicle_ai / ai.enable_air_ai)";
		return 0;
	}
	if (function->second.function == nullptr) {
		r_message = "'" + p_create_function_name + "' has no create function";
		return 0;
	}
	return (*function->second.function)(id, p_name, p_mode, urge_curve(p_urge_curve_name), p_weights);
}

std::string BotBehaviourManager::create_uc_constant(const std::string &p_name, float p_value) {
	urge_curves_[p_name] = std::make_unique<UCConstant>(p_value);
	return "UCConstant created";
}

std::string BotBehaviourManager::create_uc_linear(const std::string &p_name, float p_slope, float p_offset) {
	urge_curves_[p_name] = std::make_unique<UCLinear>(p_slope, p_offset);
	return "UCLinear created";
}

std::string BotBehaviourManager::create_uc_x_inverse(
		const std::string &p_name, float p_numerator, float p_offset, float p_slope, float p_bias) {
	urge_curves_[p_name] = std::make_unique<UCXInverse>(p_numerator, p_offset, p_slope, p_bias);
	return "UCXInverse created";
}

const UrgeCurve *BotBehaviourManager::urge_curve(const std::string &p_name) const {
	const auto found = urge_curves_.find(p_name);
	return found != urge_curves_.end() ? found->second.get() : nullptr;
}

std::unique_ptr<BAPEntry> BotBehaviourManager::create_plan_entry(std::string_view p_name) const {
	return ai::create_plan_entry(p_name, settings_);
}

}
