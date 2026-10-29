#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <cstdint>
#include <string>

namespace bf3::ai {

class Bot;

inline constexpr int kMaxPlayerIds = 256;

class AiGame {
public:
	virtual ~AiGame() = default;
	virtual bool is_playing() const = 0;
	virtual int max_number_of_players() const = 0;
	virtual bool is_valid_player_id(int p_player_id) const = 0;
	virtual int player_side(int p_player_id) const = 0;
	virtual bool player_is_ai(int p_player_id) const = 0;
	virtual bool add_bot(int &r_player_id, int &r_side, std::string &r_name) = 0;
	virtual void terminate_player(int p_player_id) = 0;
	virtual void give_damage(AiObject *p_target, int p_from_player_id, int p_side, float p_damage) = 0;
	virtual float recommended_view_distance() const = 0;
};

class StrategicAi {
public:
	virtual ~StrategicAi() = default;
	virtual void add_bot(Bot *p_bot) = 0;
	virtual void remove_bot(Bot *p_bot) = 0;
	virtual void update(double p_budget) = 0;
};

class SquadUpdater {
public:
	virtual ~SquadUpdater() = default;
	virtual void update(sim::Tick p_tick) = 0;
	virtual void new_squad_leader(int p_side, int p_squad) = 0;
	virtual void player_joins_squad(int p_player_id, int p_squad, int p_side) = 0;
};

class LevelListener {
public:
	virtual ~LevelListener() = default;
	virtual void post_load_init_areas() = 0;
};

class PathfindingService {
public:
	virtual ~PathfindingService() = default;
	virtual void update_pathfinding(double p_budget) = 0;
};

}
