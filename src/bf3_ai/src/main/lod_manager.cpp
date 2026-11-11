#include "bf3/ai/main/lod_manager.h"

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/main/bot_manager.h"

#include <cmath>

namespace bf3::ai {

void PlayerLodStats::update(const LodViewer &p_viewer, float p_basic_distance, float p_speed_scale) {
	float speed = 0.0f;
	const AiObject *const vehicle = p_viewer.alive ? p_viewer.vehicle : nullptr;
	if (vehicle != nullptr) {
		position = vehicle->position();
		if (const AiObjectMobile *const mobile = vehicle->mobile()) {
			const Vec3 &v = mobile->speed();
			speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		}
	} else {
		position = p_viewer.camera;
	}
	lod_distance = p_basic_distance + p_speed_scale * speed;
	lod_distance *= p_viewer.field_of_view_modifier;
}

AILODManager::AILODManager(const LodEnvironment &p_environment, const BotManager &p_bots,
		const InformationManager &p_information, float p_basic_lod_distance, float p_speed_scale) :
		environment_(p_environment), bots_(p_bots), information_(p_information),
		basic_lod_distance_(p_basic_lod_distance), max_lod_distance_(p_basic_lod_distance),
		speed_scale_(p_speed_scale) {
	update_all_players();
}

bool AILODManager::reset() {
	update_players();
	player_stats_.clear();
	lod_enable_ = true;
	return true;
}

bool AILODManager::lod_enable(bool p_enable) {
	lod_enable_ = p_enable;
	return p_enable;
}

void AILODManager::update_all_players() {
	if (!lod_enable_) {
		return;
	}
	updated_player_ids_.clear();
	for (const int id : environment_.active_player_ids()) {
		update_player(id);
	}
	update_player_map();
	updated_player_ids_.clear();
}

void AILODManager::update_players() {
	if (environment_.is_valid_player_id(player_cursor_)) {
		update_player(player_cursor_);
		player_cursor_ = environment_.next_player_id(player_cursor_);
		return;
	}
	const std::vector<int> players = environment_.active_player_ids();
	if (!players.empty()) {
		player_cursor_ = players.front();
		update_player_map();
	}
	updated_player_ids_.clear();
}

bool AILODManager::update_player(int p_player_id) {
	if (!lod_enable_) {
		return true;
	}
	LodViewer viewer;
	if (!environment_.viewer(p_player_id, viewer)) {
		return true;
	}
	player_stats_[p_player_id].update(viewer, basic_lod_distance_, speed_scale_);
	updated_player_ids_.insert(p_player_id);
	return true;
}

void AILODManager::update_player_map() {
	if (updated_player_ids_.size() == player_stats_.size()) {
		return;
	}
	for (auto it = player_stats_.begin(); it != player_stats_.end();) {
		if (updated_player_ids_.count(it->first) == 0) {
			it = player_stats_.erase(it);
		} else {
			++it;
		}
	}
}

void AILODManager::update_bots() {
	const int count = bots_.bot_count();
	if (count == 0) {
		return;
	}
	Bot *const first = bots_.bot(uint32_t(bot_cursor_));
	++bot_cursor_;
	if (!lod_enable_) {
		first->set_lod_level(BotLodLevel::Full);
	} else {
		int updated = update_bot(*first) ? 1 : 0;
		while (updated < 4 && bot_cursor_ < count) {
			const bool success = update_bot(*bots_.bot(uint32_t(bot_cursor_)));
			++bot_cursor_;
			if (success) {
				++updated;
			}
		}
	}
	if (bot_cursor_ >= count) {
		bot_cursor_ = 0;
	}
}

bool AILODManager::update_bot(Bot &r_bot) {
	const BotLodLevel old_level = r_bot.lod_level();
	r_bot.set_lod_level(BotLodLevel::Light);
	if (r_bot.is_dead()) {
		return false;
	}
	const Information *const information = information_.information(r_bot.current_equipment());
	if (information == nullptr) {
		return false;
	}
	const Vec3 &position = information->position();
	float full_threshold = 1.0f;
	float medium_threshold = 1.425f;
	if (old_level == BotLodLevel::Full) {
		full_threshold = 1.05f;
		medium_threshold = 1.5f;
	} else if (old_level == BotLodLevel::Medium) {
		full_threshold = 0.95f;
		medium_threshold = 1.575f;
	}
	const IPIMobile *const mobile = information->mobile();
	const float speed = mobile != nullptr ? mobile->speed_magnitude() : 0.0f;
	for (const auto &[id, stats] : player_stats_) {
		const float x = stats.position.x - position.x;
		const float y = stats.position.y - position.y;
		const float z = stats.position.z - position.z;
		float distance = std::sqrt(x * x + y * y + z * z) - speed * speed_scale_;
		if (stats.lod_distance > 1.0e-7f) {
			distance /= stats.lod_distance;
		}
		if (distance < full_threshold) {
			r_bot.set_lod_level(BotLodLevel::Full);
			break;
		}
		if (r_bot.lod_level() > BotLodLevel::Medium && distance < medium_threshold) {
			r_bot.set_lod_level(BotLodLevel::Medium);
		}
	}
	return true;
}

void AILODManager::remove_player(int p_player_id) {
	if (player_cursor_ == p_player_id) {
		player_cursor_ = -1;
	}
	player_stats_.erase(p_player_id);
	updated_player_ids_.erase(p_player_id);
}

}
