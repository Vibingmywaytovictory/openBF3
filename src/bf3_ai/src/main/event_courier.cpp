#include "bf3/ai/main/event_courier.h"

#include "bf3/ai/debug/draw_primitives.h"
#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/plug_ins.h"
#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/main/ai_main.h"
#include "bf3/ai/main/communication_hub.h"

namespace bf3::ai {

void AIEventCourier::event_missile_lock(AiObject *p_object) {
	if (p_object == nullptr) {
		return;
	}
	if (AiObjectUnit *const unit = p_object->unit()) {
		unit->set_missile_lock_status(1);
	}
}

void AIEventCourier::event_player_spawned(int p_player_id) {
	if (services_.main != nullptr) {
		services_.main->bot_manager().set_bot_is_alive(p_player_id);
	}
}

void AIEventCourier::event_player_killed(int p_player_id, int) {
	if (services_.main != nullptr) {
		services_.main->bot_manager().set_bot_is_dead(p_player_id);
	}
}

void AIEventCourier::event_player_created(bool p_is_ai) {
	if (services_.main != nullptr) {
		services_.main->new_player_created(p_is_ai);
	}
}

void AIEventCourier::event_player_disconnect(int p_player_id) {
	AIMain *const main = services_.main;
	if (main == nullptr) {
		return;
	}
	if (AILODManager *const lod = main->lod_manager()) {
		lod->remove_player(p_player_id);
	}
	if (main->bot_manager().bot_from_player_id(p_player_id) != nullptr) {
		main->deactivate_bot(p_player_id);
	} else {
		main->reactivate_bot(p_player_id);
	}
}

void AIEventCourier::event_player_enter_vehicle(int p_player_id, const AiObject *p_vehicle) {
	if (services_.main != nullptr) {
		services_.main->bot_manager().set_bot_changed_vehicle(p_player_id, p_vehicle);
	}
}

void AIEventCourier::event_game_start() {
	if (services_.main != nullptr) {
		services_.main->spawn_bots();
	}
}

void AIEventCourier::event_world_reset() {
	if (services_.primitives != nullptr) {
		services_.primitives->delete_all();
	}
	if (services_.hub != nullptr) {
		services_.hub->reset();
	}
}

void AIEventCourier::event_man_down(int p_player_id, AiObject &r_object) {
	if (services_.main != nullptr) {
		services_.main->bot_manager().set_bot_is_man_down(p_player_id);
	}
	const int side = r_object.side();
	if (side == 0 || services_.information == nullptr) {
		return;
	}
	const Information *const information = services_.information->information(r_object.information((side == 1) + 1));
	if (information != nullptr && information->physical() != nullptr) {
		information->physical()->set_health(0.0f);
	}
}

void AIEventCourier::event_revived(int p_player_id) {
	if (services_.main != nullptr) {
		services_.main->bot_manager().set_bot_is_revived(p_player_id);
	}
}

void AIEventCourier::event_radio_message(const RadioMessageSettings &p_settings, int p_sender, int p_receiver) {
	if (services_.hub != nullptr) {
		services_.hub->filter_and_receive_ai_radio_message(p_settings, p_sender, p_receiver, false);
	}
}

void AIEventCourier::event_mine_removed(uint32_t p_handle) {
	if (services_.main != nullptr) {
		services_.main->bot_manager().remove_mine_target(p_handle);
	}
}

void AIEventCourier::event_enemy_spotted(AiObject &r_object, int p_side) {
	if (services_.hub != nullptr) {
		services_.hub->enemy_spotted_by_human(r_object, p_side);
	}
}

void AIEventCourier::event_is_repaired(int p_player_id) {
	if (services_.main == nullptr) {
		return;
	}
	if (Bot *const bot = services_.main->bot_manager().bot_from_player_id(p_player_id)) {
		bot->set_is_repaired(true);
	}
}

bool AIEventCourier::event_received_artillery_request(int p_side, int p_player_id, const Vec3 &p_position) {
	StrategicAi *const sai = services_.main != nullptr ? services_.main->sai(p_side) : nullptr;
	return sai != nullptr && sai->player_requests_artillery(p_player_id, p_position);
}

bool AIEventCourier::event_received_uav_request(int p_side, int p_player_id, const Vec3 &p_position) {
	StrategicAi *const sai = services_.main != nullptr ? services_.main->sai(p_side) : nullptr;
	return sai != nullptr && sai->player_requests_uav(p_player_id, p_position);
}

bool AIEventCourier::event_received_supply_request(int p_side, int p_player_id, const Vec3 &p_position) {
	StrategicAi *const sai = services_.main != nullptr ? services_.main->sai(p_side) : nullptr;
	return sai != nullptr && sai->player_requests_supply(p_player_id, p_position);
}

bool AIEventCourier::event_received_vehicle_drop_request(int p_side, int p_player_id, const Vec3 &p_position) {
	StrategicAi *const sai = services_.main != nullptr ? services_.main->sai(p_side) : nullptr;
	return sai != nullptr && sai->player_requests_vehicle_drop(p_player_id, p_position);
}

void AIEventCourier::event_accepted_commander(int, int p_side) {
	if (StrategicAi *const sai = services_.main != nullptr ? services_.main->sai(p_side) : nullptr) {
		sai->set_player_is_commander(true);
	}
}

void AIEventCourier::event_resign_as_commander(int, int p_side) {
	if (StrategicAi *const sai = services_.main != nullptr ? services_.main->sai(p_side) : nullptr) {
		sai->set_player_is_commander(false);
	}
}

void AIEventCourier::event_new_squad_leader(int p_side, int p_squad) {
	if (services_.squads != nullptr) {
		services_.squads->new_squad_leader(p_side, p_squad);
	}
}

void AIEventCourier::event_joined_squad(int p_player_id, int p_squad, int p_side) {
	if (services_.squads != nullptr) {
		services_.squads->player_joins_squad(p_player_id, p_squad, p_side);
	}
}

void AIEventCourier::event_bot_team_killed(int p_player_id, int) {
	if (services_.main != nullptr) {
		services_.main->bot_manager().set_bot_team_killed(p_player_id);
	}
}

void AIEventCourier::event_level_loaded() {
	if (services_.level != nullptr) {
		services_.level->post_load_init_areas();
	}
}

void AIEventCourier::event_bot_switch_sides(int p_side) {
	if (services_.main != nullptr) {
		services_.main->switch_sides(p_side);
	}
}

void AIEventCourier::event_player_switch_sides(int p_side, int p_player_id) {
	if (services_.main != nullptr) {
		services_.main->switch_sides(p_side, p_player_id);
	}
}

}
