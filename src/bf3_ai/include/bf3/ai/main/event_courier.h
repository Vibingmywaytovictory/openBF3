#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <cstdint>

namespace bf3::ai {

class AIMain;
class CommunicationHub;
class DPManager;
class InformationManager;
class LevelListener;
class SquadUpdater;
struct RadioMessageSettings;

struct AiEventServices {
	AIMain *main = nullptr;
	SquadUpdater *squads = nullptr;
	LevelListener *level = nullptr;
	CommunicationHub *hub = nullptr;
	const InformationManager *information = nullptr;
	DPManager *primitives = nullptr;
};

class AIEventCourier {
public:
	void attach(const AiEventServices &p_services) { services_ = p_services; }

	void event_missile_lock(AiObject *p_object);
	void event_player_spawned(int p_player_id);
	void event_player_killed(int p_player_id, int p_side);
	void event_player_created(bool p_is_ai);
	void event_player_disconnect(int p_player_id);
	void event_player_enter_vehicle(int p_player_id, const AiObject *p_vehicle);
	void event_game_start();
	void event_world_reset();
	void event_man_down(int p_player_id, AiObject &r_object);
	void event_revived(int p_player_id);
	void event_radio_message(const RadioMessageSettings &p_settings, int p_sender, int p_receiver);
	void event_mine_removed(uint32_t p_handle);
	void event_enemy_spotted(AiObject &r_object, int p_side);
	void event_is_repaired(int p_player_id);
	bool event_received_artillery_request(int p_side, int p_player_id, const Vec3 &p_position);
	bool event_received_uav_request(int p_side, int p_player_id, const Vec3 &p_position);
	bool event_received_supply_request(int p_side, int p_player_id, const Vec3 &p_position);
	bool event_received_vehicle_drop_request(int p_side, int p_player_id, const Vec3 &p_position);
	void event_accepted_commander(int p_player_id, int p_side);
	void event_resign_as_commander(int p_player_id, int p_side);
	void event_new_squad_leader(int p_side, int p_squad);
	void event_joined_squad(int p_player_id, int p_squad, int p_side);
	void event_bot_team_killed(int p_player_id, int p_victim);
	void event_level_loaded();
	void event_bot_switch_sides(int p_side);
	void event_player_switch_sides(int p_side, int p_player_id);

private:
	AiEventServices services_;
};

}
