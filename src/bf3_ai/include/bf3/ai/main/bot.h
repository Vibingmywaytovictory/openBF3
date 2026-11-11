#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <cstdint>
#include <memory>
#include <string>

namespace bf3::ai {

class CommunicationAgent;
class Information;
class WeaponInfoManager;

enum class BotLodLevel : uint8_t { Full = 0, Medium = 1, Light = 2 };

class BotCallBack {
public:
	virtual ~BotCallBack() = default;
	virtual void report() = 0;
};

class Bot {
public:
	virtual ~Bot() = default;
	virtual uint32_t bot_id() const = 0;
	virtual int side() const = 0;
	virtual void set_side(int p_side) = 0;
	virtual int player_id() const = 0;
	virtual void set_player(int p_player_id) = 0;
	virtual const std::string &name() const = 0;
	virtual void set_name(const std::string &p_name) = 0;
	virtual void set_personality(const float *p_weights) = 0;
	virtual void receive_idle_order(int p_idle_behaviour) = 0;
	virtual bool is_dead() const = 0;
	virtual bool man_down() const = 0;
	virtual void set_man_down(bool p_down) = 0;
	virtual bool is_deactivated() const = 0;
	virtual void set_deactivated(bool p_deactivated) = 0;
	virtual bool in_transition_phase() const = 0;
	virtual void set_in_transition_phase(bool p_in) = 0;
	virtual BotLodLevel lod_level() const = 0;
	virtual void set_lod_level(BotLodLevel p_level) = 0;
	virtual uint32_t current_equipment() const = 0;
	virtual void set_current_equipment(uint32_t p_handle) = 0;
	virtual bool has_squad_member() const = 0;
	virtual int decision_making(double p_slice) = 0;
	virtual void plan_execution(double p_slice) = 0;
	virtual bool has_plan() const = 0;
	virtual bool has_default_plan() const = 0;
	virtual void invalidate_plan() = 0;
	virtual void update_player_action(bool p_stalled) = 0;
	virtual void rearm(const Information &p_equipment, const WeaponInfoManager &p_weapons, double p_time_increase) = 0;
	virtual void set_bot_stuck(bool p_stuck) = 0;
	virtual void see(double p_slice) = 0;
	virtual void update_sensing(double p_slice) = 0;
	virtual void heard_object(uint32_t p_information) = 0;
	virtual void update_communication() = 0;
	virtual CommunicationAgent *communication() const = 0;
	virtual bool path_search_pending() const = 0;
	virtual void update_obstacles() = 0;
	virtual bool navigation_active() const = 0;
	virtual void update_navigation_occasionally() = 0;
	virtual void reset_navigation(bool p_all) = 0;
	virtual bool search_active() const = 0;
	virtual bool search_new() const = 0;
	virtual bool search_running() const = 0;
	virtual void update_pathfinding(bool &r_first, bool *r_second) = 0;
	virtual bool update_bot_vehicle() = 0;
	virtual bool update_bot() = 0;
	virtual void reset() = 0;
	virtual void reset_assigned_areas() = 0;
	virtual void set_last_change_time(double p_time) = 0;
	virtual void set_spawn_coord(const Vec3 &p_position) = 0;
	virtual bool c4_placed() const = 0;
	virtual void remove_c4() = 0;
	virtual uint32_t mine_target() const = 0;
	virtual void set_mine_target(uint32_t p_handle) = 0;
	virtual void set_is_repaired(bool p_repaired) = 0;
	virtual BotCallBack *death_call_back() const = 0;
	virtual BotCallBack *spawn_call_back() const = 0;
};

class BotFactory {
public:
	virtual ~BotFactory() = default;
	virtual std::unique_ptr<Bot> create_bot(
			int p_player_id, uint32_t p_bot_id, const float *p_personality, int p_side) = 0;
};

}
