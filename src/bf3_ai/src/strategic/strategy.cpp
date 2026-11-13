#include "bf3/ai/strategic/strategy.h"

#include "bf3/ai/squad/squad_coordinator.h"
#include "bf3/ai/squad/squad_manager.h"

#include <algorithm>
#include <cfloat>

namespace bf3::ai {

namespace {

using core::KeyDef;
using core::KeyKind;

#define BF3_SAI_FIELD(f) [](StrategyTuning &t) -> void * { return &t.f; }

const KeyDef<StrategyTuning> kKeys[] = {
	{ "sai.period_ticks", KeyKind::U32, 1, 600, BF3_SAI_FIELD(sai_period_ticks) },
	{ "sai.squad_updates_per_tick", KeyKind::U32, 1, 64, BF3_SAI_FIELD(squad_updates_per_tick) },
	{ "sai.hunt_heat", KeyKind::Float, 0.0, 1000000.0, BF3_SAI_FIELD(hunt_heat) },
	{ "sai.hunt_keep", KeyKind::Float, 0.0, 1.0, BF3_SAI_FIELD(hunt_keep) },
	{ "sai.order.revive", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[0]) },
	{ "sai.order.fire_infantery", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[1]) },
	{ "sai.order.take_cover_infantry", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[2]) },
	{ "sai.order.avoid", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[3]) },
	{ "sai.order.move_to", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[4]) },
	{ "sai.order.random", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[5]) },
	{ "sai.order.idle", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[6]) },
	{ "sai.order.medic_assist", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[7]) },
	{ "sai.order.call_for_medic", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[8]) },
	{ "sai.order.resupply", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[9]) },
};

#undef BF3_SAI_FIELD

}

bool is_strategy_key(std::string_view p_key) {
	return p_key.substr(0, kStrategySettingsPrefix.size()) == kStrategySettingsPrefix;
}

core::KeyOutcome parse_strategy_setting(std::string_view p_key, std::string_view p_value, StrategyTuning &r_tuning) {
	const auto key = std::find_if(std::begin(kKeys), std::end(kKeys),
			[&](const KeyDef<StrategyTuning> &p_def) { return p_def.name == p_key; });
	if (key == std::end(kKeys)) {
		return { core::KeyFault::UnknownKey };
	}
	StrategyTuning scratch = r_tuning;
	const core::KeyOutcome outcome = core::parse_key_value(*key, p_value, scratch);
	if (outcome.fault == core::KeyFault::None) {
		r_tuning = scratch;
	}
	return outcome;
}

std::string format_strategy_tuning(const StrategyTuning &p_tuning) {
	return core::format_key_values(p_tuning, std::span<const KeyDef<StrategyTuning>>(kKeys));
}

void StrategyManager::reset() {
	forced_.fill(nullptr);
	strategies_.clear();
	active_strategy_name_ = std::string(kNoStrategies);
	active_strategy_ = nullptr;
	prerequisites_.clear();
	active_prerequisite_name_ = std::string(kNoPrerequisites);
	active_prerequisite_ = nullptr;
	areas_.clear();
	conditions_.clear();
	active_condition_name_.clear();
	object_modifiers_.clear();
}

StrategyCondition *StrategyManager::active_condition() const {
	return condition(active_condition_name_);
}

std::string StrategyManager::active_condition_name() const {
	return conditions_.contains(active_condition_name_) ? active_condition_name_ : std::string();
}

Strategy *StrategyManager::strategy(const std::string &p_name) const {
	const auto found = strategies_.find(p_name);
	return found != strategies_.end() ? found->second.get() : nullptr;
}

StrategyCondition *StrategyManager::condition(const std::string &p_name) const {
	const auto found = conditions_.find(p_name);
	return found != conditions_.end() ? found->second.get() : nullptr;
}

StrategyPrerequisite *StrategyManager::prerequisite(const std::string &p_name) const {
	const auto found = prerequisites_.find(p_name);
	return found != prerequisites_.end() ? found->second.get() : nullptr;
}

Strategy *StrategyManager::set_active_strategy(const std::string &p_name) {
	active_strategy_ = strategy(p_name);
	active_strategy_name_ = active_strategy_ != nullptr ? p_name : std::string(kNullStrategy);
	return active_strategy_;
}

StrategyPrerequisite *StrategyManager::set_active_prerequisite(const std::string &p_name) {
	active_prerequisite_ = prerequisite(p_name);
	active_prerequisite_name_ = active_prerequisite_ != nullptr ? p_name : std::string();
	return active_prerequisite_;
}

StrategyCondition *StrategyManager::set_active_condition(const std::string &p_name) {
	active_condition_name_ = p_name;
	return active_condition();
}

bool StrategyManager::set_prerequisite(const std::string &p_name) {
	if (active_strategy_ == nullptr) {
		return false;
	}
	active_strategy_->set_prerequisite(prerequisite(p_name));
	return true;
}

std::string StrategyManager::force_strategy(const std::string &p_name, int p_side) {
	if (p_side != 1 && p_side != 2) {
		return "Side must be 1 or 2, current side: " + std::to_string(p_side);
	}
	Strategy *const found = strategy(p_name);
	forced_[size_t(p_side - 1)] = found;
	return (found != nullptr ? "Strategy is activated : " : "No active strategy chosen : ") + p_name;
}

bool StrategyManager::create_strategy(const std::string &p_name) {
	if (strategies_.contains(p_name)) {
		active_strategy_name_ = std::string(kNullStrategy);
		active_strategy_ = nullptr;
		return false;
	}
	active_strategy_ = strategies_.emplace(p_name, std::make_unique<Strategy>(p_name)).first->second.get();
	active_strategy_name_ = p_name;
	return true;
}

bool StrategyManager::create_prerequisite(const std::string &p_name) {
	if (prerequisites_.contains(p_name)) {
		return false;
	}
	active_prerequisite_ =
			prerequisites_.emplace(p_name, std::make_unique<StrategyPrerequisite>(p_name)).first->second.get();
	active_prerequisite_name_ = p_name;
	return true;
}

bool StrategyManager::add_condition(const std::string &p_name, float p_weight) {
	if (active_prerequisite_ == nullptr) {
		return false;
	}
	active_prerequisite_->add_condition(condition(p_name), p_weight);
	return true;
}

bool StrategyManager::add_specific_object_modifier(const std::string &p_name, float p_modifier) {
	object_modifiers_.push_back({ p_name, active_strategy_, p_modifier });
	return true;
}

bool StrategyManager::add_required_preceding_strategy(const std::string &p_name) {
	return active_strategy_ != nullptr && active_strategy_->add_required_preceding_strategy(strategy(p_name));
}

bool StrategyManager::add_prohibited_preceding_strategy(const std::string &p_name) {
	return active_strategy_ != nullptr && active_strategy_->add_prohibited_preceding_strategy(strategy(p_name));
}

void StrategyManager::complete_strategies() {
	if (objects_ == nullptr) {
		return;
	}
	for (const ObjectStrategyModifier &tuple : object_modifiers_) {
		const AIStrategicArea *area = objects_->strategic_area(tuple.object_name);
		if (area != nullptr && tuple.strategy != nullptr) {
			tuple.strategy->set_specific_strategic_object_modifier(area->index(), tuple.modifier);
		}
	}
}

void StrategyManager::complete_conditions() {
	for (SCArea *area : areas_) {
		area->update_area();
	}
}

namespace {

template <class T> std::string list_names(const std::map<std::string, T> &p_map) {
	std::string out;
	for (const auto &entry : p_map) {
		out += entry.first;
		out += '\n';
	}
	return out;
}

}

std::string StrategyManager::list_strategies() const {
	return list_names(strategies_);
}

std::string StrategyManager::list_prerequisites() const {
	return list_names(prerequisites_);
}

std::string StrategyManager::list_conditions() const {
	return list_names(conditions_);
}

bool StrategyManager::insert_condition(const std::string &p_name, std::unique_ptr<StrategyCondition> p_condition) {
	if (conditions_.contains(p_name)) {
		active_condition_name_.clear();
		return false;
	}
	conditions_.emplace(p_name, std::move(p_condition));
	active_condition_name_ = p_name;
	return true;
}

bool StrategyManager::create_constant_condition(const std::string &p_name,
		StrategyCondition::CompareType p_compare_type, StrategyCondition::Compare p_compare,
		StrategyCondition::Side p_side, StrategyCondition::Conditional p_conditional, float p_constant) {
	return insert_condition(p_name,
			std::make_unique<SCConstant>(p_name, p_compare_type, p_compare, p_side, p_conditional, p_constant, 0.0f));
}

bool StrategyManager::create_homogenous_condition(const std::string &p_name,
		StrategyCondition::CompareType p_compare_type, StrategyCondition::Compare p_compare,
		StrategyCondition::Conditional p_conditional) {
	return insert_condition(
			p_name, std::make_unique<SCHomogeneous>(p_name, p_compare_type, p_compare, p_conditional, 0.0f));
}

bool StrategyManager::create_heterogeneous_condition(const std::string &p_name,
		StrategyCondition::CompareType p_compare_type, StrategyCondition::Compare p_compare,
		StrategyCondition::Side p_side, StrategyCondition::Conditional p_first,
		StrategyCondition::Conditional p_second) {
	return insert_condition(p_name,
			std::make_unique<SCHeterogeneous>(p_name, p_compare_type, p_compare, p_side, p_first, p_second, 0.0f));
}

bool StrategyManager::create_area_condition(
		const std::string &p_name, const std::string &p_area_name, StrategicStatus p_status) {
	auto area = std::make_unique<SCArea>(p_name, p_area_name, -1, p_status, objects_);
	SCArea *const raw = area.get();
	if (!insert_condition(p_name, std::move(area))) {
		return false;
	}
	areas_.push_back(raw);
	return true;
}

StrategyDecisions::StrategyDecisions(StrategyManager &r_manager, SquadManager &r_squads,
		AIStrategicObjectManager &r_objects, const StrategyHeat *p_heat, const StrategyTuning &p_tuning) :
		manager_(r_manager), squads_(r_squads), heat_(p_heat), tuning_(p_tuning) {
	for (AIStrategicObject *object : r_objects.strategic_objects()) {
		if (object->type() == 0) {
			objects_.push_back(static_cast<AIStrategicArea *>(object));
			areas_.emplace_back(*objects_.back());
		}
	}
	for (int side = 1; side <= 2; ++side) {
		squad_areas_[size_t(side - 1)].assign(size_t(squads_.coordinator_count(side)), -1);
		assigned_[size_t(side - 1)].assign(areas_.size(), 0);
		visited_[size_t(side - 1)].assign(areas_.size(), 0u);
	}
	manager_.create_strategy(std::string(kStrategyHunt));
	manager_.create_strategy(std::string(kStrategySpread));
}

float StrategyDecisions::heat(int p_side, size_t p_area) const {
	return heat_ != nullptr ? heat_->area_heat(*objects_[p_area]) : areas_[p_area].area().temperature(p_side);
}

void StrategyDecisions::choose_strategy(Sai &r_sai) {
	const int side = r_sai.side();
	float hottest = 0.0f;
	for (size_t i = 0; i < areas_.size(); ++i) {
		hottest = std::max(hottest, heat(side, i));
	}
	set_active_strategy(side, hottest >= tuning_.hunt_heat && hottest > 0.0f ? kStrategyHunt : kStrategySpread);
}

void StrategyDecisions::set_active_strategy(int p_side, std::string_view p_name) {
	const size_t s = size_t(p_side - 1);
	const Strategy *forced = manager_.forced_strategy(p_side);
	const Strategy *chosen = forced != nullptr ? forced : manager_.strategy(std::string(p_name));
	if (chosen == nullptr || chosen == active_[s]) {
		return;
	}
	active_[s] = chosen;
	++changes_[s];
}

std::string StrategyDecisions::active_strategy_name(int p_side) const {
	const Strategy *active = active_[size_t(p_side - 1)];
	return active != nullptr ? active->name() : std::string(kNoStrategies);
}

void StrategyDecisions::modify_strategic_objects_temperatures(Sai &r_sai) {
	if (heat_ == nullptr) {
		return;
	}
	for (AIStrategicArea *a : objects_) {
		a->set_temperature(r_sai.side(), heat_->area_heat(*a));
	}
}

bool StrategyDecisions::squad_position(const SquadCoordinator &p_squad, Vec3 &r_position) const {
	if (!p_squad.squad_has_position()) {
		return false;
	}
	p_squad.squad_position(r_position);
	return true;
}

int StrategyDecisions::nearest_area(const Vec3 &p_position, const std::vector<uint8_t> *p_skip) const {
	int best = -1;
	float best_d = FLT_MAX;
	for (size_t i = 0; i < areas_.size(); ++i) {
		if (p_skip != nullptr && (*p_skip)[i] != 0) {
			continue;
		}
		const float d = distance_sq(p_position, areas_[i].area().order_position(0));
		if (d < best_d) {
			best_d = d;
			best = int(i);
		}
	}
	return best;
}

int StrategyDecisions::least_visited_area(
		int p_side, const Vec3 &p_position, const std::vector<uint8_t> &p_skip) const {
	const std::vector<uint32_t> &visited = visited_[size_t(p_side - 1)];
	int best = -1;
	uint32_t best_visit = UINT32_MAX;
	float best_d = FLT_MAX;
	for (size_t i = 0; i < areas_.size(); ++i) {
		if (p_skip[i] != 0) {
			continue;
		}
		const float d = distance_sq(p_position, areas_[i].area().order_position(0));
		if (visited[i] < best_visit || (visited[i] == best_visit && d < best_d)) {
			best_visit = visited[i];
			best_d = d;
			best = int(i);
		}
	}
	return best;
}

void StrategyDecisions::update_bot_positions(Sai &r_sai) {
	const int side = r_sai.side();
	std::vector<uint32_t> &visited = visited_[size_t(side - 1)];
	const uint32_t cycle = ++visit_cycle_[size_t(side - 1)];
	std::vector<int> &where = squad_areas_[size_t(side - 1)];
	for (int q = 1; q <= squads_.coordinator_count(side); ++q) {
		const SquadCoordinator &squad = squads_.coordinator(side, q);
		Vec3 at{};
		where[size_t(q - 1)] =
				squad.number_of_squad_members() > 0 && squad_position(squad, at) ? nearest_area(at, nullptr) : -1;
		if (where[size_t(q - 1)] >= 0) {
			visited[size_t(where[size_t(q - 1)])] = cycle;
		}
	}
}

int StrategyDecisions::squad_area(int p_side, int p_squad) const {
	const std::vector<int> &where = squad_areas_[size_t(p_side - 1)];
	return p_squad >= 1 && size_t(p_squad) <= where.size() ? where[size_t(p_squad - 1)] : -1;
}

void StrategyDecisions::update_distribution(Sai &r_sai) {
	const int side = r_sai.side();
	std::vector<uint8_t> &assigned = assigned_[size_t(side - 1)];
	std::fill(assigned.begin(), assigned.end(), uint8_t(0));
	for (int q = 1; q <= squads_.coordinator_count(side); ++q) {
		const SquadCoordinator &squad = squads_.coordinator(side, q);
		const SquadArea *area = squad.squad_strategic_area_assignment();
		if (squad.mission_status() != SquadMissionStatus::None && area != nullptr) {
			for (size_t i = 0; i < areas_.size(); ++i) {
				if (&areas_[i] == area) {
					assigned[i] = uint8_t(std::min(int(assigned[i]) + 1, 255));
				}
			}
		}
	}
}

void StrategyDecisions::distribute_resources(Sai &) {}

bool StrategyDecisions::led_by_bot(const SquadCoordinator &p_squad) const {
	const SquadMember *leader = p_squad.squad_leader();
	return leader != nullptr && leader->is_bot();
}

bool StrategyDecisions::free_for_mission(const SquadCoordinator &p_squad) const {
	return p_squad.number_of_squad_members() > 0 && led_by_bot(p_squad) &&
			p_squad.mission_status() == SquadMissionStatus::None;
}

void StrategyDecisions::send(Sai &r_sai, SquadCoordinator &r_squad, size_t p_area, SquadOrder p_order) {
	const Vec3 at = areas_[p_area].area().order_position(0);
	Commander *const commander = r_sai.services().commander;
	const uint16_t target = uint16_t(p_area + 1u);
	if (commander == nullptr) {
		r_squad.assign_squad(&areas_[p_area], at, p_order, SquadMissionStatus::Issued);
	} else if (p_order == SquadOrder::Attack) {
		commander->give_attack_order(at, r_squad.squad_id() + 1, r_sai.side(), target);
	} else if (p_order == SquadOrder::Defend) {
		commander->give_defend_order(at, r_squad.squad_id() + 1, r_sai.side(), target);
	} else {
		commander->give_move_order(at, r_squad.squad_id() + 1, r_sai.side(), target);
	}
	std::vector<uint8_t> &assigned = assigned_[size_t(r_squad.side())];
	assigned[p_area] = uint8_t(std::min(int(assigned[p_area]) + 1, 255));
	++stats_.missions;
}

void StrategyDecisions::distribute_defence(Sai &r_sai) {
	const int side = r_sai.side();
	if (r_sai.have_any_owned_and_takeable_control_points(3 - side)) {
		++stats_.defence_withheld;
		return;
	}
	for (size_t i = 0; i < areas_.size(); ++i) {
		const AIStrategicArea &a = areas_[i].area();
		if (!a.is_area_takeable() || a.status(side) != StrategicStatus::Owned || assigned_[size_t(side - 1)][i] != 0 ||
				!(heat(side, i) > 0.0f || a.has_neighbour_with_status(StrategicStatus::Hostile, side))) {
			continue;
		}
		SquadCoordinator *best = nullptr;
		float best_d = FLT_MAX;
		for (int q = 1; q <= squads_.coordinator_count(side); ++q) {
			SquadCoordinator &squad = squads_.coordinator(side, q);
			Vec3 at{};
			if (!free_for_mission(squad) || !squad_position(squad, at)) {
				continue;
			}
			const float d = distance_sq(at, a.order_position(0));
			if (d < best_d) {
				best_d = d;
				best = &squad;
			}
		}
		if (best != nullptr) {
			send(r_sai, *best, i, SquadOrder::Defend);
			++stats_.defend_missions;
		}
	}
}

void StrategyDecisions::distribute_attack(Sai &r_sai) {
	const int side = r_sai.side();
	for (int q = 1; q <= squads_.coordinator_count(side); ++q) {
		SquadCoordinator &squad = squads_.coordinator(side, q);
		Vec3 at{};
		if (!free_for_mission(squad) || !squad_position(squad, at)) {
			continue;
		}
		int best = -1;
		float best_cost = FLT_MAX;
		for (size_t i = 0; i < areas_.size(); ++i) {
			const AIStrategicArea &a = areas_[i].area();
			if (!a.is_area_takeable() || a.status(side) == StrategicStatus::Owned) {
				continue;
			}
			const float cost = distance(at, a.order_position(0)) * float(1 + assigned_[size_t(side - 1)][i]);
			if (cost < best_cost) {
				best_cost = cost;
				best = int(i);
			}
		}
		if (best >= 0 && areas_[size_t(best)].area().enemy_takeover(side)) {
			send(r_sai, squad, size_t(best), SquadOrder::Defend);
			++stats_.defend_missions;
		} else if (best >= 0) {
			send(r_sai, squad, size_t(best), SquadOrder::Attack);
			++stats_.attack_missions;
		}
	}
}

void StrategyDecisions::distribute_with_no_areas(Sai &r_sai) {
	const int side = r_sai.side();
	if (areas_.empty()) {
		return;
	}
	const Strategy *active = active_[size_t(side - 1)];
	if (active != nullptr && active->name() == kStrategyHunt) {
		size_t hottest = 0;
		for (size_t i = 1; i < areas_.size(); ++i) {
			if (heat(side, i) > heat(side, hottest)) {
				hottest = i;
			}
		}
		int &hunted = hunted_[size_t(side - 1)];
		if (hunted < 0 || heat(side, size_t(hunted)) < tuning_.hunt_keep * heat(side, hottest)) {
			hunted = int(hottest);
		}
		for (int q = 1; q <= squads_.coordinator_count(side); ++q) {
			SquadCoordinator &squad = squads_.coordinator(side, q);
			if (squad.number_of_squad_members() == 0 || !led_by_bot(squad)) {
				continue;
			}
			const SquadArea *on = squad.squad_strategic_area_assignment();
			const bool assigned = squad.mission_status() != SquadMissionStatus::None && on != nullptr;
			const bool there = assigned && on->index() == areas_[size_t(hunted)].index();
			if (squad_area(side, q) == hunted) {
				if (assigned && !there) {
					squad.un_assign_squad();
				}
				continue;
			}
			if (!there) {
				send(r_sai, squad, size_t(hunted), SquadOrder::Move);
				++stats_.hunt_missions;
			}
		}
		return;
	}
	hunted_[size_t(side - 1)] = -1;
	std::vector<uint8_t> taken = assigned_[size_t(side - 1)];
	for (int q = 1; q <= squads_.coordinator_count(side); ++q) {
		const int here = squad_area(side, q);
		if (here >= 0 && squads_.coordinator(side, q).number_of_squad_members() > 0) {
			taken[size_t(here)] = 1;
		}
	}
	for (int q = 1; q <= squads_.coordinator_count(side); ++q) {
		SquadCoordinator &squad = squads_.coordinator(side, q);
		Vec3 at{};
		if (!free_for_mission(squad) || !squad_position(squad, at)) {
			continue;
		}
		std::vector<uint8_t> skip = taken;
		const int here = squad_area(side, q);
		if (here >= 0) {
			skip[size_t(here)] = 1;
		}
		int next = least_visited_area(side, at, skip);
		if (next < 0) {
			std::fill(skip.begin(), skip.end(), uint8_t(0));
			if (here >= 0) {
				skip[size_t(here)] = 1;
			}
			next = least_visited_area(side, at, skip);
		}
		if (next >= 0) {
			send(r_sai, squad, size_t(next), SquadOrder::Move);
			taken[size_t(next)] = 1;
			++stats_.spread_missions;
		}
	}
}

int StrategyDecisions::release_squads_from_object(Sai &, int p_object, int p_side) {
	int released = 0;
	for (int q = 1; q <= squads_.coordinator_count(p_side); ++q) {
		SquadCoordinator &squad = squads_.coordinator(p_side, q);
		const SquadArea *area = squad.squad_strategic_area_assignment();
		if (area != nullptr && area->index() == p_object && squad.mission_status() != SquadMissionStatus::None) {
			squad.un_assign_squad();
			++released;
		}
	}
	stats_.released += uint64_t(released);
	return released;
}

}
