#include "bf3/ai/main/ai_main.h"

#include <algorithm>

namespace bf3::ai {

AIMain::AIMain(const AiMainServices &p_services) : services_(p_services) {
	BotManagerServices bots;
	bots.settings = services_.settings;
	bots.timer = services_.timer;
	bots.information = services_.information;
	bots.behaviours = services_.behaviours;
	bots.game = services_.game;
	bots.factory = services_.factory;
	bots.pathfinding = services_.pathfinding;
	bot_manager_ = std::make_unique<BotManager>(*this, bots);
}

std::string AIMain::init(size_t p_strategic_object_count) {
	initialised_ = true;
	set_sai_enable(true, p_strategic_object_count);
	if (services_.settings != nullptr && services_.game != nullptr && services_.settings->view_distance_m <= 0.0f) {
		services_.settings->view_distance_m = services_.game->recommended_view_distance();
	}
	return "AIMain initiated.";
}

bool AIMain::action() {
	const double tick_seconds = services_.timer != nullptr ? services_.timer->tick_seconds() : 0.0;
	const double start = services_.timer != nullptr ? services_.timer->update_time() : 0.0;
	const double budget = tick_seconds * ai_system_quotient_;
	const double deadline = start + budget;
	for (Bot *const bot : pending_bots_) {
		if (StrategicAi *const strategist = sai(bot->side())) {
			strategist->add_bot(bot);
		}
	}
	pending_bots_.clear();
	const uint32_t sides = services_.settings != nullptr ? services_.settings->sides : 0;
	if (sai_enabled_ && sides > 0) {
		if (StrategicAi *const strategist = sai(int(component_cursor_) + 1)) {
			strategist->update(budget * component_quotient_ * 0.95);
		}
		component_cursor_ = (component_cursor_ + 1) % sides;
	}
	if (services_.squads != nullptr && services_.timer != nullptr) {
		services_.squads->update(services_.timer->game_tick());
	}
	const double now = services_.timer != nullptr ? services_.timer->update_time() : deadline;
	bot_manager_->action(deadline - now);
	return true;
}

void AIMain::spawn_bots() {
	AiGame *const game = services_.game;
	const AiSettings *const settings = services_.settings;
	if (game == nullptr || settings == nullptr || !game->is_playing() || !settings->auto_spawn_bots) {
		return;
	}
	int wanted = settings->max_bots;
	if (settings->max_bots_include_humans) {
		wanted = std::min(wanted, game->max_number_of_players()) - human_count_;
	}
	for (int missing = wanted - bot_manager_->number_of_active_bots(); missing > 0; --missing) {
		int player_id = -1;
		int side = 0;
		std::string name;
		if (!game->add_bot(player_id, side, name)) {
			break;
		}
		activate_bot(player_id, side, name);
	}
}

bool AIMain::activate_bot(int p_player_id, int p_side, const std::string &p_name) {
	if (services_.game == nullptr || !services_.game->is_valid_player_id(p_player_id)) {
		return false;
	}
	if (bot_manager_->reactivate_bot(p_player_id, p_side, p_name)) {
		return true;
	}
	return add_unspawned_bot(p_player_id, p_side, nullptr);
}

bool AIMain::add_unspawned_bot(int p_player_id, int p_side, const float *p_personality) {
	if (services_.settings == nullptr || bot_manager_->bot_count() >= services_.settings->max_bots) {
		return false;
	}
	if (!bot_manager_->add_unspawned_bot(p_player_id, p_side, p_personality)) {
		return false;
	}
	Bot *const bot = bot_manager_->bot(uint32_t(bot_manager_->bot_count() - 1));
	if (StrategicAi *const strategist = sai(p_side)) {
		strategist->add_bot(bot);
	}
	return true;
}

void AIMain::new_player_created(bool p_is_ai) {
	if (p_is_ai) {
		++bot_count_;
		return;
	}
	++human_count_;
	AiGame *const game = services_.game;
	if (services_.settings == nullptr || game == nullptr || !services_.settings->max_bots_include_humans ||
			bot_count_ <= 0 || human_count_ + bot_count_ <= game->max_number_of_players()) {
		return;
	}
	int players_on_side[2] = { 0, 0 };
	int candidate[2] = { -1, -2 };
	for (int id = 0; id < kMaxPlayerIds; ++id) {
		const int side = game->player_side(id);
		if (side == 1 || side == 2) {
			++players_on_side[side - 1];
			if (game->player_is_ai(id)) {
				candidate[side - 1] = id;
			}
		}
	}
	int pick = 2;
	if (players_on_side[0] > players_on_side[1]) {
		pick = 1;
	}
	if (candidate[pick - 1] < 0) {
		pick = (pick == 1) + 1;
	}
	const int victim = candidate[pick - 1];
	if (victim < 0) {
		return;
	}
	game->terminate_player(victim);
	--bot_count_;
}

void AIMain::switch_sides(int p_side, int p_player_id) {
	Bot *const bot = bot_manager_->bot_from_player_id(p_player_id);
	if (bot == nullptr || bot->side() == 0 || bot->side() == p_side || p_side == 0) {
		return;
	}
	if (StrategicAi *const old_strategist = sai((p_side == 1) + 1)) {
		old_strategist->remove_bot(bot);
	}
	bot->set_side(p_side);
	if (StrategicAi *const new_strategist = sai(p_side)) {
		new_strategist->add_bot(bot);
	}
}

void AIMain::set_sai(int p_side, StrategicAi *p_sai) {
	if (p_side >= 0 && size_t(p_side) < sai_.size()) {
		sai_[size_t(p_side)] = p_sai;
	}
}

StrategicAi *AIMain::sai(int p_side) const {
	return p_side >= 0 && size_t(p_side) < sai_.size() ? sai_[size_t(p_side)] : nullptr;
}

bool AIMain::set_sai_enable(bool p_enable, size_t p_strategic_object_count) {
	if (p_strategic_object_count > 0) {
		sai_enabled_ = p_enable;
		return p_enable;
	}
	sai_enabled_ = false;
	return false;
}

void AIMain::queue_pending_bot(Bot *p_bot) {
	pending_bots_.push_back(p_bot);
}

}
