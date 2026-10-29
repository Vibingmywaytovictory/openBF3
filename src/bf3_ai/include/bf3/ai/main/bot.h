#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace bf3::ai {

enum class BotLodLevel : uint8_t { Full = 0, Medium = 1, Light = 2 };

class Bot {
public:
	virtual ~Bot() = default;
	virtual uint32_t bot_id() const = 0;
	virtual int side() const = 0;
	virtual void set_side(int p_side) = 0;
	virtual int player_id() const = 0;
	virtual void set_player(int p_player_id) = 0;
	virtual void set_name(const std::string &p_name) = 0;
	virtual void set_personality(const float *p_weights) = 0;
	virtual void receive_idle_order(int p_idle_behaviour) = 0;
	virtual bool is_dead() const = 0;
	virtual bool man_down() const = 0;
	virtual bool is_deactivated() const = 0;
	virtual void set_deactivated(bool p_deactivated) = 0;
	virtual BotLodLevel lod_level() const = 0;
	virtual uint32_t current_equipment() const = 0;
	virtual bool has_squad_member() const = 0;
	virtual int decision_making(double p_slice) = 0;
	virtual void see(double p_slice) = 0;
	virtual void update_sensing(double p_slice) = 0;
	virtual void update_communication() = 0;
	virtual bool path_search_pending() const = 0;
	virtual void update_obstacles() = 0;
	virtual bool navigation_active() const = 0;
	virtual void update_navigation_occasionally() = 0;
	virtual bool search_active() const = 0;
	virtual bool search_new() const = 0;
	virtual bool search_running() const = 0;
	virtual void update_pathfinding(bool &r_first, bool *r_second) = 0;
};

class BotFactory {
public:
	virtual ~BotFactory() = default;
	virtual std::unique_ptr<Bot> create_bot(
			int p_player_id, uint32_t p_bot_id, const float *p_personality, int p_side) = 0;
};

}
