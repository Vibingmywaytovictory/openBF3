#include "bf3/ai/strategic/sai.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/information_grid.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/main/ai_settings.h"
#include "bf3/ai/main/bot.h"
#include "bf3/ai/squad/squad_manager.h"

#include <algorithm>

namespace bf3::ai {

namespace {

constexpr AIStrategicObject::Type kAreaFlags[] = { AIStrategicObject::Flank, AIStrategicObject::Base,
	AIStrategicObject::Close, AIStrategicObject::Centre, AIStrategicObject::Remote };
constexpr AIStrategicObject::Type kOtherFlags[] = { AIStrategicObject::Route, AIStrategicObject::Bridge,
	AIStrategicObject::Route, AIStrategicObject::Bridge, AIStrategicObject::North, AIStrategicObject::West,
	AIStrategicObject::South, AIStrategicObject::East };
constexpr int kOtherIndices[] = { 27, 28, 27, 28, 29, 30, 31, 32 };

}

Sai::Sai(int p_side, const SaiServices &p_services) :
		services_(p_services), side_(p_side), side_mask_(1 << p_side), enemy_mask_(p_side == 1 ? 4 : 2) {
	clear_states(state_own_[0], state_enemy_[0]);
	clear_states(state_own_[1], state_enemy_[1]);
	const int bots = services_.settings != nullptr ? std::max(services_.settings->max_bots, 0) : 0;
	bot_info_.resize(size_t(bots));
}

void Sai::clear_states(std::array<float, kSaiStateCount> &r_own, std::array<float, kSaiStateCount> &r_enemy) {
	r_own.fill(0.0f);
	r_enemy.fill(0.0f);
	r_own[kSaiStateCount - 1] = 1.0f;
	r_enemy[kSaiStateCount - 1] = 1.0f;
}

double Sai::game_time() const {
	return services_.timer != nullptr ? double(services_.timer->game_tick()) * services_.timer->tick_seconds() : 0.0;
}

bool Sai::over_budget(double p_start, double p_budget) const {
	return services_.timer != nullptr && services_.timer->update_time() - p_start > p_budget * 0.95;
}

void Sai::add_bot(Bot *p_bot) {
	if (p_bot == nullptr) {
		return;
	}
	if (services_.squads != nullptr) {
		services_.squads->add_squad_member(p_bot->player_id(), side_);
	}
	added_.push_back(p_bot);
}

void Sai::remove_bot(Bot *p_bot) {
	if (p_bot == nullptr) {
		return;
	}
	added_.erase(std::remove(added_.begin(), added_.end(), p_bot), added_.end());
	if (services_.squads != nullptr) {
		services_.squads->remove_squad_member(p_bot->player_id(), side_);
	}
	removed_.push_back(p_bot);
}

void Sai::set_new_bot_kit(int p_bot_id, int p_kit) {
	const auto found = dead_bots_.find(p_bot_id);
	if (found != dead_bots_.end()) {
		found->second.second = p_kit;
	}
}

void Sai::add_spawn_to_point(int p_point) {
	const auto inserted = spawns_to_points_.insert({ p_point, 1 });
	if (!inserted.second) {
		++inserted.first->second;
	}
}

int Sai::spawns_to_point(int p_point) const {
	const auto found = spawns_to_points_.find(p_point);
	return found == spawns_to_points_.end() ? 0 : found->second;
}

bool Sai::is_player_commander(int p_side) const {
	return p_side == side_ && player_commander_;
}

bool Sai::have_any_owned_control_points(int p_side) const {
	if (services_.objects == nullptr) {
		return false;
	}
	for (const AIStrategicObject *object : services_.objects->strategic_objects()) {
		const auto *area = object->type() == 0 ? static_cast<const AIStrategicArea *>(object) : nullptr;
		if (area != nullptr && area->control_point() != kNoControlPoint &&
				area->status(p_side) == StrategicStatus::Owned) {
			return true;
		}
	}
	return false;
}

bool Sai::have_any_owned_and_takeable_control_points(int p_side) const {
	if (services_.objects == nullptr) {
		return false;
	}
	for (const AIStrategicObject *object : services_.objects->strategic_objects()) {
		const auto *area = object->type() == 0 ? static_cast<const AIStrategicArea *>(object) : nullptr;
		if (area != nullptr && area->is_area_takeable() && area->status(p_side) == StrategicStatus::Owned) {
			return true;
		}
	}
	return false;
}

bool Sai::own_all_takeable_control_points(int p_side) const {
	if (services_.objects == nullptr) {
		return false;
	}
	bool any = false;
	for (const AIStrategicObject *object : services_.objects->strategic_objects()) {
		const auto *area = object->type() == 0 ? static_cast<const AIStrategicArea *>(object) : nullptr;
		if (area == nullptr || !area->is_area_takeable()) {
			continue;
		}
		any = true;
		if (area->status(p_side) != StrategicStatus::Owned) {
			return false;
		}
	}
	return any;
}

void Sai::update(double p_budget) {
	const double start = services_.timer != nullptr ? services_.timer->update_time() : 0.0;
	const float increase = services_.timer != nullptr ? float(services_.timer->tick_seconds()) : 0.0f;
	for (BotInfo &info : bot_info_) {
		if (info.bot != nullptr) {
			info.time_alive += increase;
		}
	}
	SaiDecisions *decide = player_commander_ ? nullptr : services_.decisions;
	switch (stage_) {
		case SaiStage::Done:
			stage_ = SaiStage::Start;
			[[fallthrough]];
		case SaiStage::Start:
			stage_ = SaiStage::Strengths;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Strengths:
			update_strengths();
			stage_ = SaiStage::ClearStates;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::ClearStates:
			clear_states(state_own_[current_ ^ 1u], state_enemy_[current_ ^ 1u]);
			stage_ = SaiStage::UpdateStates;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::UpdateStates:
			update_states(state_own_[current_ ^ 1u], state_enemy_[current_ ^ 1u]);
			current_ ^= 1u;
			stage_ = SaiStage::Prepare;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Prepare: {
			const double frequency = services_.settings != nullptr ? services_.settings->sai_update_frequency : 35.0;
			if (!(game_time() - last_update_ > frequency)) {
				return;
			}
			prepare_update();
			stage_ = SaiStage::StrategicObjects;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		}
		case SaiStage::StrategicObjects:
			if (!player_commander_ && !update_strategic_objects()) {
				return;
			}
			stage_ = SaiStage::ChooseStrategy;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::ChooseStrategy:
			if (decide != nullptr) {
				decide->choose_strategy(*this);
			}
			stage_ = SaiStage::Temperatures;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Temperatures:
			if (services_.decisions != nullptr) {
				services_.decisions->modify_strategic_objects_temperatures(*this);
			}
			stage_ = SaiStage::BotPositions;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::BotPositions:
			if (services_.decisions != nullptr) {
				services_.decisions->update_bot_positions(*this);
			}
			stage_ = SaiStage::Distribution;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Distribution:
			if (decide != nullptr) {
				decide->update_distribution(*this);
			}
			stage_ = SaiStage::Resources;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Resources:
			if (decide != nullptr) {
				decide->distribute_resources(*this);
			}
			if (!have_any_owned_control_points(side_)) {
				stage_ = SaiStage::NoAreas;
				return;
			}
			stage_ = SaiStage::Defence;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Defence:
			if (decide != nullptr) {
				decide->distribute_defence(*this);
			}
			stage_ = SaiStage::Attack;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Attack:
			if (decide != nullptr) {
				decide->distribute_attack(*this);
			}
			stage_ = SaiStage::Artillery;
			if (over_budget(start, p_budget)) {
				return;
			}
			[[fallthrough]];
		case SaiStage::Artillery:
			stage_ = SaiStage::Done;
			++completed_;
			return;
		case SaiStage::NoAreas:
			if (decide != nullptr) {
				decide->distribute_with_no_areas(*this);
			}
			stage_ = SaiStage::Done;
			++completed_;
			return;
	}
}

void Sai::update_strengths() {
	if (services_.grid == nullptr || services_.information == nullptr) {
		return;
	}
	std::vector<uint32_t> handles;
	const UnitPredicate units;
	services_.grid->all_information(handles, &units);
	for (const uint32_t handle : handles) {
		const Information *root = services_.information->information(handle);
		if (root == nullptr) {
			continue;
		}
		const bool friendly = root->side() == side_;
		std::array<float, 6> &strengths = friendly ? friendly_strengths_ : enemy_strengths_;
		std::array<float, 6> &counts = friendly ? friendly_counts_ : enemy_counts_;
		int secondary = root->first_secondary_object_iterator();
		const Information *current = root;
		while (current != nullptr) {
			const IPIUnit *unit = current->unit();
			if (unit != nullptr && unit->is_occupied()) {
				const float security = current->security();
				for (size_t category = 0; category < strengths.size(); ++category) {
					strengths[category] = security * unit->battle_strengths()[category] + strengths[category];
				}
				const IPIPhysical *physical = current->physical();
				const int category = physical != nullptr ? physical->strength_type() : 0;
				if (category >= 0 && category < int(counts.size())) {
					counts[size_t(category)] += security;
				}
			}
			current = services_.information->information(root->secondary_object(secondary, side_));
		}
	}
	for (size_t category = 0; category < 6; ++category) {
		for (std::array<float, 6> *values :
				{ &friendly_strengths_, &friendly_counts_, &enemy_strengths_, &enemy_counts_ }) {
			if ((*values)[category] > 0.001f) {
				(*values)[category] *= 0.5f;
			}
		}
	}
}

void Sai::update_states(std::array<float, kSaiStateCount> &r_own, std::array<float, kSaiStateCount> &r_enemy) {
	const int enemy = side_ == 1 ? 2 : 1;
	if (services_.game != nullptr) {
		r_own[13] = float(services_.game->tickets(side_));
		r_enemy[13] = float(services_.game->tickets(enemy));
		r_own[14] = float(services_.game->number_of_control_points(side_));
		r_enemy[14] = float(services_.game->number_of_control_points(enemy));
	}
	const float now = float(game_time());
	r_own[15] = now;
	r_enemy[15] = now;
	if (services_.objects == nullptr) {
		return;
	}
	int attack = 0;
	int defence = 0;
	std::array<float, kSaiStateCount> *selected = nullptr;
	bool neighbour = false;
	for (const AIStrategicObject *object : services_.objects->strategic_objects()) {
		if (object->type() == 0) {
			switch (object->status(side_)) {
				case StrategicStatus::Owned:
					r_own[19] += 1.0f;
					r_enemy[21] += 1.0f;
					selected = &r_own;
					neighbour = object->has_neighbour_with_status(StrategicStatus::Hostile, side_);
					(neighbour ? r_own[39] : r_own[40]) += 1.0f;
					(neighbour ? r_enemy[41] : r_enemy[42]) += 1.0f;
					break;
				case StrategicStatus::Hostile:
					r_own[21] += 1.0f;
					r_enemy[19] += 1.0f;
					selected = &r_enemy;
					neighbour = object->has_neighbour_with_status(StrategicStatus::Owned, side_);
					(neighbour ? r_enemy[39] : r_enemy[40]) += 1.0f;
					(neighbour ? r_own[41] : r_own[42]) += 1.0f;
					break;
				case StrategicStatus::Neutral:
					r_own[20] += 1.0f;
					r_enemy[20] += 1.0f;
					if (object->has_neighbouring_object_with_status(StrategicStatus::Owned, side_)) {
						r_own[38] += 1.0f;
					}
					if (object->has_neighbouring_object_with_status(StrategicStatus::Hostile, side_)) {
						r_enemy[38] += 1.0f;
					}
					selected = nullptr;
					break;
				case StrategicStatus::Unknown:
					selected = nullptr;
					neighbour = false;
					break;
			}
		}
		if (object->number_of_assigned_units(side_) > 0) {
			++(object->is_attack(side_) ? attack : defence);
		}
		if (selected == nullptr) {
			continue;
		}
		for (size_t i = 0; i < std::size(kAreaFlags); ++i) {
			if (object->has_object_type_flag(kAreaFlags[i])) {
				(*selected)[22 + i] += 1.0f;
				if (neighbour) {
					(*selected)[33 + i] += 1.0f;
				}
			}
		}
		for (size_t i = 0; i < std::size(kOtherFlags); ++i) {
			if (object->has_object_type_flag(kOtherFlags[i])) {
				(*selected)[size_t(kOtherIndices[i])] += 1.0f;
			}
		}
	}
	r_enemy[17] = float(attack);
	r_enemy[18] = float(defence);
}

void Sai::prepare_update() {
	for (Bot *bot : added_) {
		const uint32_t id = bot->bot_id();
		if (id >= bot_info_.size()) {
			continue;
		}
		bot_info_[id] = { bot, 0.0f };
		if (bot->is_dead()) {
			dead_bots_.insert({ int(id), { -1, 13 } });
		}
	}
	added_.clear();
	for (auto &[point, count] : spawns_to_points_) {
		count = 0;
	}
	friendly_strengths_.fill(0.0f);
	friendly_counts_.fill(0.0f);
	if (services_.information != nullptr) {
		for (const BotInfo &info : bot_info_) {
			if (info.bot == nullptr || info.bot->is_dead()) {
				continue;
			}
			const Information *information = services_.information->information(info.bot->current_equipment());
			if (information == nullptr || information->unit() == nullptr) {
				continue;
			}
			for (size_t i = 0; i < 6; ++i) {
				friendly_strengths_[i] = information->unit()->battle_strengths()[i];
			}
			const int category = information->physical() != nullptr ? information->physical()->strength_type() : 0;
			if (category >= 0 && category < 6) {
				friendly_counts_[size_t(category)] += 1.0f;
			}
		}
	}
	for (auto it = dead_bots_.begin(); it != dead_bots_.end();) {
		if (it->first < 0 || size_t(it->first) >= bot_info_.size() || bot_info_[size_t(it->first)].bot == nullptr) {
			it = dead_bots_.erase(it);
		} else {
			++it;
		}
	}
	for (Bot *bot : removed_) {
		const uint32_t id = bot->bot_id();
		if (id < bot_info_.size() && bot_info_[id].bot == bot) {
			bot_info_[id] = {};
		}
	}
	removed_.clear();
}

bool Sai::update_strategic_objects() {
	if (services_.objects == nullptr || services_.objects->strategic_objects().empty()) {
		return true;
	}
	const std::vector<AIStrategicObject *> &objects = services_.objects->strategic_objects();
	if (objects_pass_start_) {
		objects_changed_ = false;
		objects_pass_start_ = false;
		object_cursor_ = 0;
		enemy_stats_.clear();
		friendly_stats_.clear();
	}
	bool changed = false;
	AIStrategicObject *object = objects[object_cursor_];
	object->update(side_, *this, changed);
	objects_changed_ = objects_changed_ || changed;
	if (object->status(side_) == StrategicStatus::Owned) {
		enemy_stats_.add(object->battle_statistics(side_));
	} else {
		friendly_stats_.add(object->battle_statistics(side_));
	}
	++object_cursor_;
	if (object_cursor_ != objects.size()) {
		return false;
	}
	if (objects_changed_) {
		for (AIStrategicObject *each : objects) {
			each->set_as_dirty(side_);
			if (services_.decisions != nullptr && !player_commander_) {
				services_.decisions->release_squads_from_object(*this, each->index(), side_);
			}
		}
	}
	objects_pass_start_ = true;
	return true;
}

}
