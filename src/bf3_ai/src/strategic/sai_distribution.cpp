#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/main/bot.h"
#include "bf3/ai/strategic/sai.h"
#include "bf3/ai/strategic/strategy_model.h"

#include <list>
#include <queue>
#include <utility>

namespace bf3::ai {

namespace {

struct SquareInfo {
	int index = -1;
	float value = 0.0f;
	bool operator<(const SquareInfo &p_other) const { return value < p_other.value; }
};

float mean_strength(const IPIUnit &p_unit) {
	float sum = 0.0f;
	for (int i = 0; i < kStrategicStrengthCount; ++i) {
		sum += p_unit.strategic_strengths()[i];
	}
	return sum / float(kStrategicStrengthCount);
}

}

const IPIUnit *Sai::current_unit(int p_bot_id) const {
	const BotInfo *info = bot_info(p_bot_id);
	if (info == nullptr || services_.information == nullptr) {
		return nullptr;
	}
	const Information *information = services_.information->information(info->bot->current_equipment());
	return information != nullptr ? information->unit() : nullptr;
}

void Sai::assign_free(BotInfo &r_info, std::set<int>::iterator &r_it, bool p_attacker) {
	const int id = int(r_info.bot->bot_id());
	if (AIStrategicObject *current = object(r_info.occupied)) {
		if (p_attacker) {
			current->register_available_attacker(id, side_);
		} else {
			current->register_available_defender(id, side_);
		}
	}
	r_it = free_bots_.erase(r_it);
	assigned_bots_.insert(id);
}

void Sai::update_distribution() {
	if (services_.objects == nullptr) {
		return;
	}
	float attack_sum = 0.0f;
	float defence_sum = 0.0f;
	released_ = false;
	attack_resources_ = 0;
	defence_resources_ = 0;
	idle_resources_ = 0;
	for (AIStrategicObject *each : services_.objects->strategic_objects()) {
		for (const bool attacker : { true, false }) {
			std::set<int> &units = each->available_bots(attacker, side_);
			if (attacker ? free_attackers_ : free_defenders_) {
				while (!units.empty()) {
					const int id = *units.begin();
					BotInfo *info = size_t(id) < bots_.size() ? bots_[size_t(id)].get() : nullptr;
					if (info != nullptr) {
						free_bot(*info, true);
					}
					units.erase(id);
					released_ = true;
				}
				continue;
			}
			for (const int id : units) {
				const IPIUnit *unit = current_unit(id);
				if (unit == nullptr) {
					continue;
				}
				(attacker ? attack_resources_ : defence_resources_) +=
						int(unit->strategic_strengths()[attacker ? 0 : 1]);
				(attacker ? attack_sum : defence_sum) += mean_strength(*unit);
			}
		}
		for (const int id : each->present_units(side_)) {
			if (const IPIUnit *unit = current_unit(id)) {
				idle_resources_ += int(mean_strength(*unit));
			}
		}
		for (const int id : each->assigned_units(side_)) {
			const IPIUnit *unit = current_unit(id);
			if (unit == nullptr) {
				continue;
			}
			if (each->is_attack(side_)) {
				attack_resources_ += int(unit->strategic_strengths()[0]);
				attack_sum += mean_strength(*unit);
			} else {
				defence_resources_ += int(unit->strategic_strengths()[1]);
				defence_sum += mean_strength(*unit);
			}
		}
	}
	defence_sum += attack_sum;
	idle_resources_ -= int(defence_sum);
}

void Sai::distribute_resources() {
	const std::array<float, kSaiStateCount> &friendly = own_state();
	float attack_share = 0.0f;
	float defence_share = 0.0f;
	if (friendly[StrategyCondition::NumberOfFriendlyAreas] == 0.0f) {
		attack_share = 1.0f;
	} else if (friendly[StrategyCondition::NumberOfNeutralAreas] == 0.0f &&
			friendly[StrategyCondition::NumberOfHostileAreas] == 0.0f) {
		defence_share = 1.0f;
	} else {
		const Strategy *strategy = active_strategy();
		attack_share = strategy != nullptr ? strategy->aggression() : 0.5f;
		defence_share = 1.0f - attack_share;
	}
	bool last_was_defence = true;
	for (auto it = free_bots_.begin(); it != free_bots_.end();) {
		BotInfo *info = size_t(*it) < bots_.size() ? bots_[size_t(*it)].get() : nullptr;
		const Information *information = info != nullptr && services_.information != nullptr
				? services_.information->information(info->bot->current_equipment())
				: nullptr;
		if (information == nullptr || information->unit() == nullptr) {
			++it;
			continue;
		}
		AIStrategicObject *at = object(info->occupied);
		if (at == nullptr) {
			at = services_.objects != nullptr ? services_.objects->closest_object(information->position(), side_)
											  : nullptr;
			if (at == nullptr) {
				++it;
				continue;
			}
			respawn(*info, *at);
		} else {
			at->set_as_present_unit(*it, side_);
		}
		if (at->index() != info->last_ordered) {
			info->order_time = game_time(services_.timer);
			info->last_ordered = at->index();
		}
		const IPIUnit &unit = *information->unit();
		const int attack = attack_resources_;
		const int defence = defence_resources_;
		bool to_attack = false;
		if (attack + defence == 0) {
			to_attack = attack_share >= 0.5f;
		} else if (0.0f >= attack_share) {
			to_attack = false;
		} else if (0.0f >= defence_share) {
			to_attack = true;
		} else {
			to_attack = attack_share >= float(attack) / float(attack + defence);
		}
		if (to_attack) {
			attack_resources_ = int(unit.strategic_strengths()[0]) + attack;
		} else {
			defence_resources_ = int(unit.strategic_strengths()[1]) + defence;
		}
		idle_resources_ -= int(mean_strength(unit));
		last_was_defence = !to_attack;
		assign_free(*info, it, to_attack);
	}
	const float total = float(attack_resources_ + defence_resources_ + idle_resources_);
	if (total == 0.0f) {
		free_attackers_ = false;
		free_defenders_ = false;
		return;
	}
	free_attackers_ = float(attack_resources_) / total > attack_share * 1.2f && !last_was_defence;
	free_defenders_ = float(defence_resources_) / total > 1.2f * defence_share && last_was_defence;
	released_ = released_ || free_attackers_ || free_defenders_;
}

void Sai::feed_least_supplied(std::vector<AIStrategicObject *> p_objects) {
	const int side = side_;
	const auto less_supplied = [side](const AIStrategicObject *p_a, const AIStrategicObject *p_b) {
		return p_a->assigned_part_of_wanted_strength(side) > p_b->assigned_part_of_wanted_strength(side);
	};
	std::priority_queue<AIStrategicObject *, std::vector<AIStrategicObject *>, decltype(less_supplied)> queue(
			less_supplied, std::move(p_objects));
	const CommanderInterface commander = commander_interface();
	while (!queue.empty()) {
		AIStrategicObject *top = queue.top();
		queue.pop();
		if (top->collect_additional_squad_resources(1, side_, commander) != 0) {
			queue.push(top);
		}
	}
}

void Sai::distribute_defence() {
	const Strategy *strategy = active_strategy();
	if (strategy == nullptr || strategy->number_of_defences() == 0 || services_.objects == nullptr) {
		return;
	}
	const AIStrategicObjectManager &manager = *services_.objects;
	if (manager.number_of_categories() == 0) {
		category_distribute_defence(manager.strategic_objects());
		return;
	}
	for (int category = 0; category < manager.number_of_categories(); ++category) {
		category_distribute_defence(manager.strategic_objects_of_category(category));
	}
}

void Sai::category_distribute_defence(const std::vector<AIStrategicObject *> &p_objects) {
	const uint32_t defences = active_strategy()->number_of_defences();
	std::list<SquareInfo> squares;
	int released = 0;
	for (AIStrategicObject *each : p_objects) {
		if (each == nullptr) {
			continue;
		}
		const float need = each->calculate_defense_need(side_);
		if (need > 0.1f) {
			squares.push_back({ each->index(), need });
		} else if (0.0f >= need && !each->is_attack(side_) && !player_commander_) {
			released += release_squads_from_object(each->index());
		}
	}
	squares.sort();
	squares.reverse();
	if (squares.empty()) {
		return;
	}
	float threshold = 0.0f;
	uint32_t rank = 0;
	auto it = squares.begin();
	for (; it != squares.end(); ++it) {
		++rank;
		if (rank == defences * 2) {
			threshold = it->value;
		} else if (rank > defences * 2 && threshold * 0.9f > it->value) {
			break;
		}
	}
	while (it != squares.end()) {
		AIStrategicObject *dropped = object(it->index);
		if (dropped != nullptr && !dropped->is_attack(side_) && !player_commander_) {
			released += release_squads_from_object(dropped->index());
		}
		it = squares.erase(it);
	}
	if (squares.empty()) {
		return;
	}
	if (free_defenders_ && !released_ && released == 0) {
		squares.reverse();
		for (const SquareInfo &square : squares) {
			AIStrategicObject *each = object(square.index);
			if (each == nullptr || each->is_attack(side_) || player_commander_) {
				continue;
			}
			released += release_squads_from_object(each->index());
			if (released > 0) {
				return;
			}
		}
		return;
	}
	std::vector<AIStrategicObject *> defended;
	for (const SquareInfo &square : squares) {
		AIStrategicObject *each = object(square.index);
		if (each == nullptr) {
			continue;
		}
		each->set_attack(side_, false);
		defended.push_back(each);
		if (!(defences > defended.size())) {
			break;
		}
	}
	feed_least_supplied(std::move(defended));
}

void Sai::distribute_attack() {
	const Strategy *strategy = active_strategy();
	if (strategy == nullptr || strategy->number_of_attacks() == 0 || services_.objects == nullptr) {
		return;
	}
	const AIStrategicObjectManager &manager = *services_.objects;
	if (manager.number_of_categories() == 0) {
		category_distribute_attack(manager.strategic_objects());
		return;
	}
	for (int category = 0; category < manager.number_of_categories(); ++category) {
		category_distribute_attack(manager.strategic_objects_of_category(category));
	}
}

void Sai::category_distribute_attack(const std::vector<AIStrategicObject *> &p_objects) {
	const uint32_t attacks = active_strategy()->number_of_attacks();
	std::list<SquareInfo> squares;
	int released = 0;
	for (AIStrategicObject *each : p_objects) {
		if (each == nullptr) {
			continue;
		}
		const float value = each->calculate_attack_value(side_);
		if (value > 0.001f) {
			squares.push_back({ each->index(), value });
		}
	}
	squares.sort();
	squares.reverse();
	if (squares.empty()) {
		return;
	}
	float threshold = 0.0f;
	uint32_t rank = 0;
	auto it = squares.begin();
	while (it != squares.end()) {
		++rank;
		if (rank <= attacks) {
			threshold = it->value;
		} else if (rank > attacks * 2 || threshold * 0.8f > it->value) {
			break;
		}
		++it;
	}
	while (it != squares.end()) {
		AIStrategicObject *dropped = object(it->index);
		if (dropped != nullptr && dropped->is_attack(side_) && !player_commander_) {
			released += release_squads_from_object(dropped->index());
		}
		it = squares.erase(it);
	}
	const bool shed = free_attackers_ && !released_ && released == 0;
	if (shed) {
		squares.pop_back();
		for (const SquareInfo &square : squares) {
			AIStrategicObject *each = object(square.index);
			if (each == nullptr || !each->is_attack(side_) || player_commander_) {
				continue;
			}
			released += release_squads_from_object(each->index());
			if (released > 0) {
				return;
			}
		}
		return;
	}
	std::list<SquareInfo> chosen;
	for (it = squares.begin(); it != squares.end() && !(chosen.size() > attacks);) {
		AIStrategicObject *each = object(it->index);
		if (each != nullptr && each->is_attack(side_)) {
			chosen.push_back(*it);
			it = squares.erase(it);
		} else {
			++it;
		}
	}
	for (it = squares.begin(); !(chosen.size() > attacks) && it != squares.end(); ++it) {
		chosen.push_back(*it);
	}
	const CommanderInterface commander = commander_interface();
	std::vector<AIStrategicObject *> attacked;
	for (const SquareInfo &square : chosen) {
		AIStrategicObject *each = object(square.index);
		if (each == nullptr) {
			continue;
		}
		each->set_attack(side_, true);
		each->collect_squad_resources(side_, commander);
		attacked.push_back(each);
	}
	feed_least_supplied(std::move(attacked));
}

void Sai::distribute_with_no_areas() {
	if (services_.objects == nullptr) {
		return;
	}
	const AIStrategicObjectManager &manager = *services_.objects;
	if (manager.number_of_categories() == 0) {
		category_distribute_with_no_areas(manager.strategic_objects());
		return;
	}
	for (int category = 0; category < manager.number_of_categories(); ++category) {
		category_distribute_with_no_areas(manager.strategic_objects_of_category(category));
	}
}

void Sai::category_distribute_with_no_areas(const std::vector<AIStrategicObject *> &p_objects) {
	const CommanderInterface commander = commander_interface();
	for (AIStrategicObject *each : p_objects) {
		if (each == nullptr) {
			continue;
		}
		each->set_attack(side_, each->status(side_) != StrategicStatus::Owned);
		each->collect_additional_squad_resources(2, side_, commander);
	}
}

}
