#pragma once

#include "bf3/ai/main/bot.h"

#include <cstdint>
#include <string>

namespace bf3::ai::test {

struct NullBot : Bot {
	uint32_t id = 0;
	int team = 1;
	int player = -1;
	std::string bot_name;
	uint32_t equipment = kNoHandle;
	BotLodLevel lod = BotLodLevel::Full;
	bool dead = false;
	bool down = false;
	bool deactivated = false;
	bool transition = false;
	bool squad_member = false;
	bool planned = false;
	uint32_t mine = kNoHandle;

	uint32_t bot_id() const override { return id; }
	int side() const override { return team; }
	void set_side(int p_side) override { team = p_side; }
	int player_id() const override { return player; }
	void set_player(int p_player_id) override { player = p_player_id; }
	const std::string &name() const override { return bot_name; }
	void set_name(const std::string &p_name) override { bot_name = p_name; }
	void set_personality(const float *) override {}
	void receive_idle_order(int) override {}
	bool is_dead() const override { return dead; }
	bool man_down() const override { return down; }
	void set_man_down(bool p_down) override { down = p_down; }
	bool is_deactivated() const override { return deactivated; }
	void set_deactivated(bool p_deactivated) override { deactivated = p_deactivated; }
	bool in_transition_phase() const override { return transition; }
	void set_in_transition_phase(bool p_in) override { transition = p_in; }
	BotLodLevel lod_level() const override { return lod; }
	void set_lod_level(BotLodLevel p_level) override { lod = p_level; }
	uint32_t current_equipment() const override { return equipment; }
	void set_current_equipment(uint32_t p_handle) override { equipment = p_handle; }
	bool has_squad_member() const override { return squad_member; }
	int decision_making(double) override { return 0; }
	void plan_execution(double) override {}
	bool has_plan() const override { return planned; }
	bool has_default_plan() const override { return false; }
	void invalidate_plan() override { planned = false; }
	void update_player_action(bool) override {}
	void rearm(const Information &, const WeaponInfoManager &, double) override {}
	void set_bot_stuck(bool) override {}
	void see(double) override {}
	void update_sensing(double) override {}
	void heard_object(uint32_t) override {}
	void update_communication() override {}
	CommunicationAgent *communication() const override { return nullptr; }
	bool path_search_pending() const override { return false; }
	void update_obstacles() override {}
	bool navigation_active() const override { return false; }
	void update_navigation_occasionally() override {}
	void reset_navigation(bool) override {}
	bool search_active() const override { return false; }
	bool search_new() const override { return false; }
	bool search_running() const override { return false; }
	void update_pathfinding(bool &, bool *) override {}
	bool update_bot_vehicle() override { return true; }
	bool update_bot() override { return true; }
	void reset() override {}
	void reset_assigned_areas() override {}
	void set_last_change_time(double) override {}
	void set_spawn_coord(const Vec3 &) override {}
	bool c4_placed() const override { return false; }
	void remove_c4() override {}
	uint32_t mine_target() const override { return mine; }
	void set_mine_target(uint32_t p_handle) override { mine = p_handle; }
	void set_is_repaired(bool) override {}
	BotCallBack *death_call_back() const override { return nullptr; }
	BotCallBack *spawn_call_back() const override { return nullptr; }
};

}
