#include "bf3/ai/main/bot_manager.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/main/ai_main.h"
#include "bf3/ai/main/behaviour_manager.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

BotManager::BotManager(AIMain &r_main, const BotManagerServices &p_services) : main_(r_main), services_(p_services) {
	for (uint8_t i = 0; i <= 4; ++i) {
		updates_.push_back(Update(i));
	}
	set_lod_level_ticks(6, 6, 6);
	set_lod_level_priority(3, 3, 3);
	set_planned_decision_making_threshold(0.5, 0.5, 0.5);
	set_unplanned_decision_making_threshold(0.3, 0.4, 0.4);
	set_decision_making_interleave(2, 2);
	set_sensing_quotient(1, 1);
	set_system_quotient(30, 40, 30);
	if (services_.settings != nullptr && services_.factory != nullptr && services_.settings->max_bots != -1) {
		for (int i = 0; i < services_.settings->max_bots; ++i) {
			std::unique_ptr<Bot> bot = services_.factory->create_bot(-1, uint32_t(i), nullptr, 1);
			if (bot) {
				free_bots_.push_back(std::move(bot));
			}
		}
	}
	bot_by_player_id_.resize(kMaxPlayerIds, -1);
}

BotManager::~BotManager() = default;

double BotManager::now() const {
	return services_.timer != nullptr ? services_.timer->update_time() : 0.0;
}

void BotManager::action(double p_budget) {
	const double deadline = now() + p_budget;
	update_damages();
	action_com_agent();
	const double share = deadline - now();
	action_update_nav_points();
	double remaining = deadline - now();
	auto update = updates_.begin();
	for (; update != updates_.end() && remaining > 0.0; ++update) {
		switch (*update) {
			case Update::Idle:
			case Update::Hearing:
				break;
			case Update::DecisionMaking:
				action_decision_making(std::min(share * 0.2, remaining));
				break;
			case Update::Pathfinding:
				action_pathfinding(std::min(share * 0.3, remaining));
				break;
			case Update::Sensing:
				action_sensing(std::min(share * 0.15, remaining));
				break;
		}
		remaining = deadline - now();
	}
	if (update == updates_.end()) {
		return;
	}
	int moves = 5;
	auto front = updates_.begin();
	while (front != updates_.end() && *front != *update && moves > 0) {
		updates_.push_back(*front);
		front = updates_.erase(front);
		--moves;
	}
}

void BotManager::update_damages() {
	for (const std::unique_ptr<BotInfo> &info : infos_) {
		if (!info->bot->is_dead() && info->damage > 0.0f && services_.game != nullptr &&
				size_t(info->attacker_index) < infos_.size()) {
			const Bot *const attacker = infos_[size_t(info->attacker_index)]->bot;
			const uint32_t handle = info->bot->current_equipment();
			const Information *const information = (handle & kHandleIndexMask) != 0 && services_.information != nullptr
					? services_.information->information(handle)
					: nullptr;
			if (information != nullptr) {
				services_.game->give_damage(
						information->ai_object(), attacker->player_id(), attacker->side(), info->damage);
			}
		}
		info->damage = 0.0f;
	}
}

void BotManager::action_com_agent() {
	if (infos_.empty()) {
		return;
	}
	const uint32_t start = com_agent_cursor_;
	for (;;) {
		Bot *const bot = infos_[com_agent_cursor_]->bot;
		const bool ready = !bot->is_dead() && !bot->man_down() && !bot->path_search_pending();
		if (++com_agent_cursor_ >= infos_.size()) {
			com_agent_cursor_ = 0;
		}
		if (ready) {
			bot->update_communication();
			return;
		}
		if (com_agent_cursor_ == start) {
			return;
		}
	}
}

void BotManager::action_update_nav_points() {
	if (infos_.empty()) {
		return;
	}
	const uint32_t limit = uint32_t(infos_.size() >> 4) + 1;
	uint32_t updated = 0;
	uint32_t visited = 0;
	while (updated < limit && visited < infos_.size()) {
		Bot *const bot = infos_[nav_point_cursor_]->bot;
		nav_point_cursor_ = (nav_point_cursor_ + 1) % uint32_t(infos_.size());
		++visited;
		if (bot == nullptr || bot->is_dead() || bot->man_down()) {
			continue;
		}
		bot->update_obstacles();
		if (bot->navigation_active()) {
			bot->update_navigation_occasionally();
			++updated;
		}
	}
}

uint32_t BotManager::lod_ticks(BotLodLevel p_level) const {
	switch (p_level) {
		case BotLodLevel::Full:
			return lod_full_ticks_;
		case BotLodLevel::Medium:
			return lod_medium_ticks_;
		case BotLodLevel::Light:
			return lod_light_ticks_;
	}
	return 0;
}

bool BotManager::has_equipment(const Bot &p_bot) const {
	return services_.information != nullptr && services_.information->information(p_bot.current_equipment()) != nullptr;
}

void BotManager::action_decision_making(double p_slice) {
	const double deadline = now() + p_slice;
	const int spare = std::max(0, int(planned_decision_making_interleave_) - int(planned_queue_.size()));
	const uint32_t unplanned_limit = unplanned_decision_making_interleave_ + uint32_t(spare);
	uint32_t weight = 0;
	selected_bots_.resize(0);
	while (!unplanned_queue_.empty() && selected_bots_.size() < unplanned_limit) {
		BotInfo *const info = unplanned_queue_.top();
		unplanned_queue_.pop();
		weight += lod_ticks(info->bot->lod_level());
		selected_bots_.push_back(info);
	}
	uint32_t planned = 0;
	while (!planned_queue_.empty() && planned++ < planned_decision_making_interleave_) {
		BotInfo *const info = planned_queue_.top();
		planned_queue_.pop();
		weight += lod_ticks(info->bot->lod_level());
		selected_bots_.push_back(info);
	}
	if (selected_bots_.empty() || weight == 0) {
		return;
	}
	const double per_tick = p_slice / double(weight);
	for (BotInfo *const info : selected_bots_) {
		if (info->bot == nullptr || info->bot->is_dead() || !has_equipment(*info->bot)) {
			continue;
		}
		const double bot_slice = double(lod_ticks(info->bot->lod_level())) * per_tick;
		if (bot_slice > 0.0) {
			const double start = now();
			info->behaviour_update_count += info->bot->decision_making(bot_slice);
			const double end = now();
			++info->decision_updates;
			info->since_decision = 0.0;
			info->decision_run_time += end - start;
		}
		info->priority = 0;
		if (now() > deadline) {
			return;
		}
	}
}

void BotManager::action_pathfinding(double p_slice) {
	const double start = now();
	const double deadline = start + p_slice;
	const double bot_deadline = start + p_slice * 0.1;
	const uint32_t size = uint32_t(infos_.size());
	for (uint32_t visited = 0; visited < size; ++visited) {
		if (now() > bot_deadline) {
			break;
		}
		Bot *const bot = infos_[pathfinding_cursor_]->bot;
		if (bot != nullptr && !bot->is_dead() && !bot->is_deactivated() && bot->has_squad_member() &&
				bot->search_active() && bot->search_new() && services_.information != nullptr) {
			const Information *const equipment = services_.information->information(bot->current_equipment());
			const IPIMobile *const mobile = equipment != nullptr ? equipment->mobile() : nullptr;
			if (mobile != nullptr && mobile->vehicle_number() >= 0) {
				bool first = false;
				bool second = false;
				bot->update_pathfinding(first, &second);
			}
		}
		pathfinding_cursor_ = (pathfinding_cursor_ + 1) % size;
	}
	if (services_.pathfinding != nullptr) {
		services_.pathfinding->update_pathfinding((deadline - bot_deadline) * 0.9);
	}
	for (uint32_t i = 0; i < size; ++i) {
		Bot *const bot = infos_[i]->bot;
		if (bot != nullptr && !bot->is_dead() && !bot->is_deactivated() && bot->has_squad_member() &&
				bot->search_active() && bot->search_running()) {
			bool first = false;
			bot->update_pathfinding(first, nullptr);
		}
	}
}

void BotManager::action_sensing(double p_slice) {
	const double deadline = now() + p_slice;
	selected_bots_.resize(0);
	uint32_t taken = 0;
	while (!sensing_queue_.empty() && taken <= 1) {
		BotInfo *const info = sensing_queue_.top();
		sensing_queue_.pop();
		if (!has_equipment(*info->bot)) {
			continue;
		}
		selected_bots_.push_back(info);
		++taken;
	}
	const uint32_t weight = lod_full_ticks_ * uint32_t(selected_bots_.size());
	if (weight == 0) {
		return;
	}
	const double per_tick = p_slice / double(weight);
	for (BotInfo *const info : selected_bots_) {
		const double share = per_tick * double(lod_full_ticks_);
		if (share > 0.0) {
			const double start = now();
			info->bot->see(share * sensing_quotient_);
			info->bot->update_sensing(share * sensing_quotient_inverse_);
			const double end = now();
			++info->sensing_updates;
			info->sensing_priority = 0;
			info->sensing_run_time += end - start;
		}
		if (now() > deadline) {
			break;
		}
	}
}

int BotManager::bot_count() const {
	return int(infos_.size());
}

int BotManager::number_of_active_bots() const {
	return int(infos_.size()) - int(deactivated_bots_.size());
}

int BotManager::number_of_deactivated_bots() const {
	return int(deactivated_bots_.size());
}

Bot *BotManager::bot(uint32_t p_index) const {
	return infos_[p_index]->bot;
}

Bot *BotManager::bot_from_id(uint32_t p_id) const {
	return p_id < infos_.size() && infos_[p_id] ? infos_[p_id]->bot : nullptr;
}

Bot *BotManager::bot_from_player_id(int p_player_id) const {
	if (p_player_id < 0 || size_t(p_player_id) >= bot_by_player_id_.size() ||
			bot_by_player_id_[size_t(p_player_id)] == -1) {
		return nullptr;
	}
	return bot_from_id(uint32_t(bot_by_player_id_[size_t(p_player_id)]));
}

Bot *BotManager::last_created_bot() const {
	return last_created_bot_;
}

bool BotManager::add_unspawned_bot(int p_player_id, int p_side, const float *p_personality) {
	last_created_bot_ = nullptr;
	if (p_player_id < 0) {
		return false;
	}
	const int idle = services_.behaviours != nullptr ? services_.behaviours->behaviour_id("Idle") : -1;
	std::unique_ptr<Bot> bot;
	if (free_bots_.empty()) {
		if (services_.factory == nullptr) {
			return false;
		}
		bot = services_.factory->create_bot(p_player_id, uint32_t(infos_.size()), p_personality, p_side);
		if (!bot) {
			return false;
		}
	} else {
		bot = std::move(free_bots_.front());
		free_bots_.pop_front();
		bot->set_player(p_player_id);
		bot->set_side(p_side);
		bot->set_personality(p_personality);
	}
	bot->receive_idle_order(idle);
	Bot *const created = bot.get();
	infos_.push_back(std::make_unique<BotInfo>(created));
	bots_.push_back(std::move(bot));
	if (bot_by_player_id_.size() <= size_t(p_player_id)) {
		bot_by_player_id_.resize(size_t(p_player_id) + 1, -1);
	}
	bot_by_player_id_[size_t(p_player_id)] = int(created->bot_id());
	last_created_bot_ = created;
	return true;
}

bool BotManager::reactivate_bot(int p_player_id, int p_side, const std::string &p_name) {
	if (deactivated_bots_.empty() || p_player_id < 0) {
		return false;
	}
	const uint32_t bot_id = deactivated_bots_.front();
	Bot *const bot = infos_[bot_id]->bot;
	bot->set_player(p_player_id);
	bot->set_side(p_side);
	bot->set_name(p_name);
	bot->set_deactivated(false);
	if (bot_by_player_id_.size() <= size_t(p_player_id)) {
		bot_by_player_id_.resize(size_t(p_player_id) + 1, -1);
	}
	bot_by_player_id_[size_t(p_player_id)] = int(bot_id);
	if (StrategicAi *const strategist = main_.sai(bot->side())) {
		strategist->add_bot(bot);
	}
	deactivated_bots_.pop_front();
	return true;
}

bool BotManager::request_decision(uint32_t p_index, bool p_planned, int p_priority) {
	if (p_index >= infos_.size()) {
		return false;
	}
	BotInfo *const info = infos_[p_index].get();
	info->priority = p_priority;
	(p_planned ? planned_queue_ : unplanned_queue_).push(info);
	return true;
}

bool BotManager::request_sensing(uint32_t p_index, int p_priority) {
	if (p_index >= infos_.size()) {
		return false;
	}
	BotInfo *const info = infos_[p_index].get();
	info->sensing_priority = p_priority;
	sensing_queue_.push(info);
	return true;
}

void BotManager::set_lod_level_ticks(uint32_t p_full, uint32_t p_medium, uint32_t p_light) {
	lod_full_ticks_ = p_full;
	lod_medium_ticks_ = p_medium;
	lod_light_ticks_ = p_light;
}

void BotManager::set_lod_level_priority(uint32_t p_full, uint32_t p_medium, uint32_t p_light) {
	lod_full_priority_ = p_full;
	lod_medium_priority_ = p_medium;
	lod_light_priority_ = p_light;
}

void BotManager::set_planned_decision_making_threshold(double p_full, double p_medium, double p_light) {
	planned_threshold_full_ = std::abs(p_full);
	planned_threshold_medium_ = std::abs(p_medium);
	planned_threshold_light_ = std::abs(p_light);
}

void BotManager::set_unplanned_decision_making_threshold(double p_full, double p_medium, double p_light) {
	unplanned_threshold_full_ = std::abs(p_full);
	unplanned_threshold_medium_ = std::abs(p_medium);
	unplanned_threshold_light_ = std::abs(p_light);
}

void BotManager::set_decision_making_interleave(uint32_t p_planned, uint32_t p_unplanned) {
	planned_decision_making_interleave_ = p_planned;
	unplanned_decision_making_interleave_ = p_unplanned;
}

void BotManager::set_sensing_quotient(uint32_t p_see, uint32_t p_sense) {
	if (p_see + p_sense != 0) {
		const double quotient = double(p_see) / double(p_see + p_sense);
		sensing_quotient_ = quotient;
		sensing_quotient_inverse_ = 1.0 - quotient;
	}
}

void BotManager::set_system_quotient(uint32_t p_decision, uint32_t p_pathfinding, uint32_t p_sensing) {
	const uint32_t total = p_decision + p_pathfinding + p_sensing;
	if (total != 0) {
		system_decision_making_quotient_ = double(p_decision) / double(total);
		system_pathfinding_quotient_ = double(p_pathfinding) / double(total);
	}
}

}
