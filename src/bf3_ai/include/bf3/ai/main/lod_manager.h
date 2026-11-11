#pragma once

#include "bf3/ai/knowledge/ai_object.h"
#include "bf3/ai/main/bot.h"

#include <map>
#include <set>
#include <vector>

namespace bf3::ai {

class BotManager;
class InformationManager;

struct LodViewer {
	bool alive = false;
	const AiObject *vehicle = nullptr;
	Vec3 camera{};
	float field_of_view_modifier = 1.0f;
};

class LodEnvironment {
public:
	virtual ~LodEnvironment() = default;
	virtual std::vector<int> active_player_ids() const = 0;
	virtual bool is_valid_player_id(int p_player_id) const = 0;
	virtual int next_player_id(int p_player_id) const = 0;
	virtual bool viewer(int p_player_id, LodViewer &r_viewer) const = 0;
};

struct PlayerLodStats {
	Vec3 position{};
	float lod_distance = 0.0f;

	void update(const LodViewer &p_viewer, float p_basic_distance, float p_speed_scale);
};

class AILODManager {
public:
	AILODManager(const LodEnvironment &p_environment, const BotManager &p_bots, const InformationManager &p_information,
			float p_basic_lod_distance, float p_speed_scale);

	bool reset();
	void set_basic_lod_distance(float p_distance) { basic_lod_distance_ = p_distance; }
	float basic_lod_distance() const { return basic_lod_distance_; }
	float max_lod_distance() const { return max_lod_distance_; }
	bool lod_enable(bool p_enable);
	bool lod_enabled() const { return lod_enable_; }
	void update_all_players();
	void update_players();
	bool update_player(int p_player_id);
	void update_player_map();
	void update_bots();
	bool update_bot(Bot &r_bot);
	void remove_player(int p_player_id);
	const std::map<int, PlayerLodStats> &player_stats() const { return player_stats_; }

private:
	const LodEnvironment &environment_;
	const BotManager &bots_;
	const InformationManager &information_;
	std::map<int, PlayerLodStats> player_stats_;
	std::set<int> updated_player_ids_;
	float basic_lod_distance_ = 0.0f;
	float max_lod_distance_ = 0.0f;
	float speed_scale_ = 0.0f;
	bool lod_enable_ = true;
	int player_cursor_ = -1;
	int bot_cursor_ = 0;
};

}
