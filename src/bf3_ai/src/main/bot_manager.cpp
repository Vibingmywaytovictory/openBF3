#include "bf3/ai/main/bot_manager.h"

#include "bf3/ai/bot/communication_agent.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/main/ai_main.h"
#include "bf3/ai/main/behaviour_manager.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

namespace {

inline constexpr int kRadioRevived = 8;
inline constexpr int kRadioTeamKilled = 9;
inline constexpr float kNeverChanged = -1000000.0f;

bool speed_squared_at_most(const Vec3 &p_speed, float p_limit) {
	const double x = p_speed.x;
	const double y = p_speed.y;
	const double z = p_speed.z;
	return (x * x + y * y) + z * z <= p_limit;
}

}

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

double BotManager::time_increase() const {
	return services_.timer != nullptr ? services_.timer->tick_seconds() : 0.0;
}

Information *BotManager::equipment_of(const Bot &p_bot) const {
	return services_.information != nullptr ? services_.information->information(p_bot.current_equipment()) : nullptr;
}

void BotManager::action(double p_budget) {
	const double deadline = now() + p_budget;
	update_damages();
	action_com_agent();
	const double share = deadline - now();
	action_execute_plan(share * 0.6);
	action_update_nav_points();
	double remaining = deadline - now();
	auto update = updates_.begin();
	for (; update != updates_.end() && remaining > 0.0; ++update) {
		switch (*update) {
			case Update::Idle:
				break;
			case Update::DecisionMaking:
				action_decision_making(std::min(share * 0.2, remaining));
				break;
			case Update::Pathfinding:
				action_pathfinding(std::min(share * 0.3, remaining));
				break;
			case Update::Hearing:
				action_hearing(std::min(share * 0.025, remaining));
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

void BotManager::action_execute_plan(double p_slice) {
	const double deadline = now() + p_slice;
	planned_queue_ = BotQueue();
	unplanned_queue_ = BotQueue();
	sensing_queue_ = SensingQueue();
	plan_executions_this_tick_ = 0;
	const uint32_t size = uint32_t(infos_.size());
	if (size == 0) {
		return;
	}
	plan_half_toggle_ = !plan_half_toggle_;
	uint32_t count = std::min(size / 2 + 1, size);
	if (!plan_half_toggle_ && (size & 1u) != 0) {
		--count;
	}
	const auto advance = [&]() { plan_cursor_ = (plan_cursor_ + 1) % size; };
	uint32_t processed = 0;
	while (processed < count) {
		BotInfo *const info = infos_[plan_cursor_].get();
		Bot *const bot = info->bot;
		if (bot == nullptr || bot->is_dead() || bot->is_deactivated() || !bot->has_squad_member()) {
			++processed;
			advance();
			continue;
		}
		const Information *const information = equipment_of(*bot);
		if (information == nullptr) {
			++processed;
			advance();
			continue;
		}
		if (bot->path_search_pending()) {
			AiGame *const game = services_.game;
			const AiObject *const vehicle = game != nullptr ? game->player_vehicle(bot->player_id()) : nullptr;
			if (vehicle != nullptr && vehicle->primary_object() != nullptr) {
				bot->set_bot_stuck(true);
			}
			const bool on_foot = game != nullptr && game->player_default_vehicle(bot->player_id()) == vehicle;
			const IPIMobile *const mobile = information->mobile();
			const IPIPhysical *const physical = information->physical();
			if (mobile == nullptr || (speed_squared_at_most(mobile->speed(), 0.25f) && on_foot) ||
					(physical != nullptr && !physical->is_in_air() && physical->is_in_water())) {
				if (game != nullptr) {
					game->respawn_bot(bot->player_id());
				}
				++processed;
				advance();
				bot->update_player_action(false);
				continue;
			}
		}
		bool executed = false;
		if (information->has_armament() && services_.weapons != nullptr) {
			bot->rearm(*information, *services_.weapons, time_increase());
		}
		info->since_decision += time_increase();
		const double start = now();
		const bool in_budget = deadline > start;
		if (in_budget) {
			bot->plan_execution(0.0);
			info->plan_execution_run_time += now() - start;
			++info->plan_execution_updates;
			executed = true;
		}
		info->priority += int(lod_full_priority_);
		info->sensing_priority += int(lod_full_priority_);
		++plan_executions_this_tick_;
		if (bot->has_plan() && !bot->has_default_plan()) {
			if (info->since_decision > planned_threshold_full_) {
				planned_queue_.push(info);
			}
		} else if (info->since_decision > unplanned_threshold_full_) {
			unplanned_queue_.push(info);
		}
		sensing_queue_.push(info);
		bot->update_player_action(!in_budget);
		if (!executed) {
			break;
		}
		++processed;
		advance();
	}
	for (uint32_t i = 0; i < size - processed; ++i) {
		infos_[plan_cursor_]->bot->update_player_action(false);
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
	return equipment_of(p_bot) != nullptr;
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
				bot->search_active() && bot->search_new()) {
			const Information *const equipment = equipment_of(*bot);
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

void BotManager::action_hearing(double p_slice) {
	if (services_.game == nullptr) {
		return;
	}
	const double deadline = now() + p_slice;
	occupied_objects_.resize(0);
	services_.game->occupied_objects(occupied_objects_);
	auto next = occupied_objects_.begin();
	while (next != occupied_objects_.end() && next->first != hearing_resume_id_) {
		++next;
	}
	if (next == occupied_objects_.end()) {
		next = occupied_objects_.begin();
	}
	const int count = int(occupied_objects_.size());
	const double time = game_time(services_.timer);
	for (int visited = 0; visited < count;) {
		AiObject *const object = next->second;
		if (++next == occupied_objects_.end()) {
			next = occupied_objects_.begin();
		}
		++visited;
		if (object->side() == 0) {
			continue;
		}
		const AiObjectArmament *const armament = object->armament();
		const AiObjectMobile *const mobile = object->mobile();
		if (armament == nullptr && mobile == nullptr) {
			continue;
		}
		float weapon_radius = 0.0f;
		float radius = 0.0f;
		if (armament != nullptr) {
			const AiWeapon *const weapon = armament->last_fired_weapon();
			if (weapon != nullptr && 3.0 > time - double(weapon->time_of_firing()) * time_increase()) {
				weapon_radius = weapon->sound_sphere_radius();
			}
		}
		if (mobile != nullptr) {
			radius = std::max(weapon_radius, mobile->sound_sphere_radius());
		}
		if (radius <= 0.1f) {
			continue;
		}
		const float radius_squared = radius * radius;
		const int side = object->side();
		const Vec3 &position = object->position();
		for (const std::pair<int, AiObject *> &listener : occupied_objects_) {
			Bot *const bot = bot_from_object(listener.second);
			if (bot == nullptr || bot->is_dead() || bot->man_down() || bot->side() == side) {
				continue;
			}
			const Vec3 &other = listener.second->position();
			const double dx = double(position.x) - other.x;
			const double dy = double(position.y) - other.y;
			const double dz = double(position.z) - other.z;
			if (!((dx * dx + dy * dy) + dz * dz > radius_squared)) {
				bot->heard_object(services_.game->force_information(*object, bot->side(), false));
			}
		}
		if (now() > deadline) {
			break;
		}
	}
	if (count > 0) {
		hearing_resume_id_ = next->first;
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

Bot *BotManager::bot_from_name(const std::string &p_name) const {
	for (const std::unique_ptr<BotInfo> &info : infos_) {
		if (info->bot->name() == p_name) {
			return info->bot;
		}
	}
	return nullptr;
}

Bot *BotManager::bot_from_object(const AiObject *p_object) const {
	const auto found = vehicle_bots_.find(p_object);
	return found != vehicle_bots_.end() ? bot_from_id(found->second) : nullptr;
}

Bot *BotManager::last_created_bot() const {
	return last_created_bot_;
}

Bot *BotManager::take_bot(int p_player_id, int p_side, const float *p_personality) {
	const int idle = services_.behaviours != nullptr ? services_.behaviours->behaviour_id("Idle") : -1;
	std::unique_ptr<Bot> bot;
	if (free_bots_.empty()) {
		if (services_.factory == nullptr) {
			return nullptr;
		}
		bot = services_.factory->create_bot(p_player_id, uint32_t(infos_.size()), p_personality, p_side);
		if (!bot) {
			return nullptr;
		}
	} else {
		bot = std::move(free_bots_.front());
		free_bots_.pop_front();
		bot->set_player(p_player_id);
		bot->set_side(p_side);
		bot->set_personality(p_personality);
	}
	bot->receive_idle_order(idle);
	Bot *const taken = bot.get();
	bots_.push_back(std::move(bot));
	return taken;
}

void BotManager::map_player(int p_player_id, const Bot &p_bot) {
	if (bot_by_player_id_.size() <= size_t(p_player_id)) {
		bot_by_player_id_.resize(size_t(p_player_id) + 1, -1);
	}
	bot_by_player_id_[size_t(p_player_id)] = int(p_bot.bot_id());
}

bool BotManager::exit_add_bot(int p_player_id) {
	if (services_.game != nullptr) {
		services_.game->terminate_player(p_player_id);
	}
	return false;
}

bool BotManager::add_bot(int p_player_id, int p_side, const float *p_personality, AiObject *p_object) {
	if (services_.settings == nullptr || int(infos_.size()) >= services_.settings->max_bots || p_player_id < 0 ||
			bot_from_player_id(p_player_id) != nullptr) {
		return exit_add_bot(p_player_id);
	}
	last_created_bot_ = nullptr;
	AiGame *const game = services_.game;
	if (game == nullptr) {
		return false;
	}
	if (p_object != nullptr && p_object->unit() == nullptr) {
		return exit_add_bot(p_player_id);
	}
	if (game->player_vehicle(p_player_id) == nullptr) {
		return exit_add_bot(p_player_id);
	}
	Bot *const created = take_bot(p_player_id, p_side, p_personality);
	if (created == nullptr) {
		return exit_add_bot(p_player_id);
	}
	created->update_bot_vehicle();
	created->update_bot();
	Information *const information = equipment_of(*created);
	if (information == nullptr) {
		return exit_add_bot(p_player_id);
	}
	information->set_strength(information->strength() + 1.0f);
	infos_.push_back(std::make_unique<BotInfo>(created));
	map_player(p_player_id, *created);
	last_created_bot_ = created;
	return true;
}

bool BotManager::add_unspawned_bot(int p_player_id, int p_side, const float *p_personality) {
	last_created_bot_ = nullptr;
	if (p_player_id < 0) {
		return false;
	}
	Bot *const created = take_bot(p_player_id, p_side, p_personality);
	if (created == nullptr) {
		return false;
	}
	infos_.push_back(std::make_unique<BotInfo>(created));
	map_player(p_player_id, *created);
	last_created_bot_ = created;
	return true;
}

int BotManager::deactivate_bot(int p_side) {
	if (infos_.empty()) {
		return -1;
	}
	uint32_t index = 0;
	bool found = false;
	for (uint32_t i = 0; i < infos_.size() && !found; ++i) {
		const Bot *const candidate = infos_[i]->bot;
		if (candidate != nullptr && candidate->side() == p_side && candidate->is_dead()) {
			index = i;
			found = true;
		}
	}
	for (uint32_t i = 0; i < infos_.size() && !found; ++i) {
		const Bot *const candidate = infos_[i]->bot;
		if (candidate != nullptr && candidate->side() == p_side) {
			index = i;
			found = true;
		}
	}
	Bot *const chosen = infos_[index]->bot;
	if (services_.game != nullptr) {
		services_.game->set_bot_name_unused(chosen->name());
	}
	deactivated_bots_.push_back(index);
	if (chosen->player_id() >= 0 && size_t(chosen->player_id()) < bot_by_player_id_.size()) {
		bot_by_player_id_[size_t(chosen->player_id())] = -1;
	}
	chosen->set_deactivated(true);
	return int(index);
}

int BotManager::deactivate_bot_from_player_id(int p_player_id) {
	Bot *const found = bot_from_player_id(p_player_id);
	if (found == nullptr) {
		return -1;
	}
	if (services_.game != nullptr) {
		services_.game->set_bot_name_unused(found->name());
	}
	const uint32_t bot_id = found->bot_id();
	if (!found->is_dead()) {
		return -1;
	}
	deactivated_bots_.push_back(bot_id);
	found->set_deactivated(true);
	bot_by_player_id_[size_t(p_player_id)] = -1;
	return int(bot_id);
}

bool BotManager::reactivate_bot(int p_player_id, int p_side, const std::string &p_name) {
	if (deactivated_bots_.empty() || p_player_id < 0) {
		return false;
	}
	const uint32_t bot_id = deactivated_bots_.front();
	Bot *const reactivated = infos_[bot_id]->bot;
	reactivated->set_player(p_player_id);
	reactivated->set_side(p_side);
	reactivated->set_name(p_name);
	reactivated->set_deactivated(false);
	map_player(p_player_id, *reactivated);
	if (StrategicAi *const strategist = main_.sai(reactivated->side())) {
		strategist->add_bot(reactivated);
	}
	deactivated_bots_.pop_front();
	return true;
}

void BotManager::update_bot_vehicle(const Bot &p_bot, const AiObject *p_vehicle) {
	const uint32_t bot_id = p_bot.bot_id();
	for (auto it = vehicle_bots_.begin(); it != vehicle_bots_.end(); ++it) {
		if (it->second == bot_id) {
			vehicle_bots_.erase(it);
			break;
		}
	}
	vehicle_bots_.emplace(p_vehicle, bot_id);
}

bool BotManager::sai_enabled() const {
	return main_.sai_enabled();
}

void BotManager::report_changed_equipment(const Bot &p_bot) {
	if (!sai_enabled()) {
		return;
	}
	const int bot_id = int(p_bot.bot_id());
	if (StrategicAi *const strategist = main_.sai(p_bot.side())) {
		strategist->set_bot_has_changed_equipment(bot_id);
	}
}

void BotManager::set_bot_changed_vehicle(int p_player_id, const AiObject *) {
	Bot *const changed = bot_from_player_id(p_player_id);
	if (changed == nullptr || changed->in_transition_phase() || changed->is_dead()) {
		return;
	}
	changed->update_bot_vehicle();
	changed->update_bot();
	changed->invalidate_plan();
	report_changed_equipment(*changed);
}

void BotManager::set_bot_is_dead(int p_player_id) {
	Bot *const died = bot_from_player_id(p_player_id);
	if (died == nullptr) {
		return;
	}
	for (auto it = vehicle_bots_.begin(); it != vehicle_bots_.end(); ++it) {
		if (it->second == uint32_t(p_player_id)) {
			vehicle_bots_.erase(it);
			break;
		}
	}
	died->set_man_down(false);
	died->set_in_transition_phase(true);
	died->reset_navigation(true);
	died->reset_assigned_areas();
	BotInfo &info = *infos_[died->bot_id()];
	info.dead = true;
	info.bot->reset();
	died->set_current_equipment(kNoHandle);
	if (died->c4_placed()) {
		died->remove_c4();
	}
	if (BotCallBack *const call_back = died->death_call_back()) {
		call_back->report();
	}
}

void BotManager::set_bot_is_alive(int p_player_id) {
	Bot *const spawned = bot_from_player_id(p_player_id);
	if (spawned == nullptr || !spawned->in_transition_phase()) {
		return;
	}
	spawned->set_in_transition_phase(false);
	spawned->update_bot_vehicle();
	spawned->update_bot();
	spawned->set_last_change_time(kNeverChanged);
	BotInfo &info = *infos_[spawned->bot_id()];
	info.priority = 0;
	info.since_decision = 0.0;
	info.dead = false;
	info.pathfinding = false;
	if (const Information *const information = equipment_of(*spawned)) {
		spawned->set_spawn_coord(information->position());
	}
	if (BotCallBack *const call_back = spawned->spawn_call_back()) {
		call_back->report();
	}
}

void BotManager::set_bot_is_man_down(int p_player_id) {
	Bot *const down = bot_from_player_id(p_player_id);
	if (down == nullptr) {
		return;
	}
	down->set_man_down(true);
	down->update_bot();
	down->update_bot_vehicle();
	down->invalidate_plan();
	down->reset_navigation(true);
}

void BotManager::set_bot_is_revived(int p_player_id) {
	Bot *const revived = bot_from_player_id(p_player_id);
	if (revived == nullptr) {
		return;
	}
	revived->set_man_down(false);
	revived->update_bot();
	revived->reset_navigation(false);
	revived->invalidate_plan();
	if (CommunicationAgent *const agent = revived->communication()) {
		agent->enqueue_radio_message(kRadioRevived, kRadioChannelVehicle, 2u, 1.0f, -1, game_time(services_.timer));
	}
	report_changed_equipment(*revived);
}

void BotManager::set_bot_received_radio_message(const RadioMessageSettings &p_settings, int p_sender, int p_player_id) {
	Bot *const receiver = bot_from_player_id(p_player_id);
	if (receiver == nullptr) {
		return;
	}
	if (CommunicationAgent *const agent = receiver->communication()) {
		agent->receive_radio_message(p_settings, p_sender, false, game_time(services_.timer));
	}
}

void BotManager::set_bot_team_killed(int p_player_id) {
	Bot *const killer = bot_from_player_id(p_player_id);
	if (killer == nullptr) {
		return;
	}
	if (CommunicationAgent *const agent = killer->communication()) {
		agent->enqueue_radio_message(kRadioTeamKilled, kRadioChannelVehicle, 3u, 0.0f, -1, game_time(services_.timer));
	}
}

void BotManager::register_damage_on_bot(float p_damage, uint32_t p_bot_index, int p_attacker_index) {
	infos_[p_bot_index]->damage += p_damage;
	infos_[p_bot_index]->attacker_index = p_attacker_index;
}

void BotManager::remove_mine_target(uint32_t p_handle) {
	for (const std::unique_ptr<BotInfo> &info : infos_) {
		if (info->bot->mine_target() == p_handle) {
			info->bot->set_mine_target(kNoHandle);
		}
	}
}

bool BotManager::reset() {
	plan_cursor_ = 0;
	infos_.clear();
	bots_.clear();
	free_bots_.clear();
	deactivated_bots_.clear();
	vehicle_bots_.clear();
	std::fill(bot_by_player_id_.begin(), bot_by_player_id_.end(), -1);
	last_created_bot_ = nullptr;
	planned_queue_ = BotQueue();
	unplanned_queue_ = BotQueue();
	set_lod_level_ticks(3, 2, 1);
	set_lod_level_priority(3, 2, 1);
	set_planned_decision_making_threshold(0.5, 1.0, 3.0);
	set_unplanned_decision_making_threshold(0.3, 0.8, 2.0);
	set_decision_making_interleave(2, 2);
	set_sensing_quotient(1, 1);
	set_system_quotient(30, 40, 30);
	hearing_resume_id_ = 0;
	return true;
}

void BotManager::reset_statistics() {
	for (uint32_t i = 0; i < infos_.size(); ++i) {
		reset_statistics(i);
	}
}

void BotManager::reset_statistics(uint32_t p_bot_index) {
	BotInfo &info = *infos_[p_bot_index];
	info.behaviour_update_count = 0;
	info.plan_execution_updates = 0;
	info.decision_updates = 0;
	info.sensing_updates = 0;
	info.plan_execution_run_time = 0.0;
	info.decision_run_time = 0.0;
	info.sensing_run_time = 0.0;
}

uint32_t BotManager::plan_execution_update_count(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->plan_execution_updates;
}

uint32_t BotManager::decision_making_update_count(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->decision_updates;
}

uint32_t BotManager::sensing_update_count(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->sensing_updates;
}

int BotManager::decision_making_behaviour_update_count(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->behaviour_update_count;
}

double BotManager::plan_execution_run_time(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->plan_execution_run_time;
}

double BotManager::decision_making_run_time(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->decision_run_time;
}

double BotManager::sensing_run_time(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->sensing_run_time;
}

bool BotManager::is_pathfinding(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->pathfinding;
}

bool BotManager::last_pathfinding_succeeded(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->last_pathfinding_succeeded;
}

bool BotManager::pathfinding_stall(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->pathfinding_stall;
}

double BotManager::last_pathfinding_run_time(uint32_t p_bot_index) const {
	return infos_[p_bot_index]->last_pathfinding_run_time;
}

uint32_t BotManager::lod_level_priority(BotLodLevel p_level) const {
	switch (p_level) {
		case BotLodLevel::Full:
			return lod_full_priority_;
		case BotLodLevel::Medium:
			return lod_medium_priority_;
		case BotLodLevel::Light:
			return lod_light_priority_;
	}
	return 0;
}

double BotManager::planned_decision_making_threshold(BotLodLevel p_level) const {
	switch (p_level) {
		case BotLodLevel::Full:
			return planned_threshold_full_;
		case BotLodLevel::Medium:
			return planned_threshold_medium_;
		case BotLodLevel::Light:
			return planned_threshold_light_;
	}
	return 0.0;
}

double BotManager::unplanned_decision_making_threshold(BotLodLevel p_level) const {
	switch (p_level) {
		case BotLodLevel::Full:
			return unplanned_threshold_full_;
		case BotLodLevel::Medium:
			return unplanned_threshold_medium_;
		case BotLodLevel::Light:
			return unplanned_threshold_light_;
	}
	return 0.0;
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
