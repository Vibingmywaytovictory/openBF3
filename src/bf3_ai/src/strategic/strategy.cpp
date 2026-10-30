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
	{ "sai.squad_spawn_draw", KeyKind::Float, 0.0, 1.0, BF3_SAI_FIELD(squad_spawn_draw) },
	{ "sai.order.revive", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[0]) },
	{ "sai.order.fire_infantery", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[1]) },
	{ "sai.order.take_cover_infantry", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[2]) },
	{ "sai.order.avoid", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[3]) },
	{ "sai.order.move_to", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[4]) },
	{ "sai.order.random", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[5]) },
	{ "sai.order.idle", KeyKind::Float, 0.0, 100.0, BF3_SAI_FIELD(order[6]) },
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

StrategyManager::StrategyManager() {
	active_name_.fill(std::string(kNoStrategies));
}

StrategyManager::~StrategyManager() {
	reset();
}

void StrategyManager::reset() {
	active_name_.fill(std::string(kNoStrategies));
	active_.fill(nullptr);
	forced_.fill(nullptr);
	strategies_.clear();
}

Strategy *StrategyManager::active_strategy(int p_side) const {
	return active_[size_t(p_side - 1)];
}

std::string StrategyManager::active_strategy_name(int p_side) const {
	return active_name_[size_t(p_side - 1)];
}

Strategy *StrategyManager::forced_strategy(int p_side) const {
	return forced_[size_t(p_side - 1)];
}

Strategy *StrategyManager::strategy(const std::string &p_name) const {
	const auto found = strategies_.find(p_name);
	return found != strategies_.end() ? found->second.get() : nullptr;
}

void StrategyManager::create_strategy(const std::string &p_name) {
	if (strategies_.find(p_name) == strategies_.end()) {
		strategies_.emplace(p_name, std::make_unique<Strategy>(p_name));
	}
}

void StrategyManager::set_active_strategy(int p_side, const std::string &p_name) {
	const size_t s = size_t(p_side - 1);
	Strategy *const chosen = forced_[s] != nullptr ? forced_[s] : strategy(p_name);
	if (chosen == nullptr || chosen == active_[s]) {
		return;
	}
	active_[s] = chosen;
	active_name_[s] = chosen->name();
	++changes_[s];
}

void StrategyManager::force_strategy(const std::string &p_name, int p_side) {
	forced_[size_t(p_side - 1)] = strategy(p_name);
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
	manager_.set_active_strategy(
			side, std::string(hottest >= tuning_.hunt_heat && hottest > 0.0f ? kStrategyHunt : kStrategySpread));
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

void StrategyDecisions::update_bot_positions(Sai &r_sai) {
	const int side = r_sai.side();
	std::vector<int> &where = squad_areas_[size_t(side - 1)];
	for (int q = 1; q <= squads_.coordinator_count(side); ++q) {
		const SquadCoordinator &squad = squads_.coordinator(side, q);
		Vec3 at{};
		where[size_t(q - 1)] =
				squad.number_of_squad_members() > 0 && squad_position(squad, at) ? nearest_area(at, nullptr) : -1;
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

void StrategyDecisions::send(SquadCoordinator &r_squad, size_t p_area, SquadOrder p_order) {
	r_squad.assign_squad(&areas_[p_area], areas_[p_area].area().order_position(0), p_order, SquadMissionStatus::Issued);
	std::vector<uint8_t> &assigned = assigned_[size_t(r_squad.side())];
	assigned[p_area] = uint8_t(std::min(int(assigned[p_area]) + 1, 255));
	++stats_.missions;
}

void StrategyDecisions::distribute_defence(Sai &r_sai) {
	const int side = r_sai.side();
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
			send(*best, i, SquadOrder::Defend);
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
		if (best >= 0) {
			send(squad, size_t(best), SquadOrder::Attack);
			++stats_.attack_missions;
		}
	}
}

void StrategyDecisions::distribute_with_no_areas(Sai &r_sai) {
	const int side = r_sai.side();
	if (areas_.empty()) {
		return;
	}
	const Strategy *active = manager_.active_strategy(side);
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
				send(squad, size_t(hunted), SquadOrder::Move);
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
		int next = nearest_area(at, &skip);
		if (next < 0) {
			std::fill(skip.begin(), skip.end(), uint8_t(0));
			if (here >= 0) {
				skip[size_t(here)] = 1;
			}
			next = nearest_area(at, &skip);
		}
		if (next >= 0) {
			send(squad, size_t(next), SquadOrder::Move);
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
