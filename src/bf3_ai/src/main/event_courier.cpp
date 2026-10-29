#include "bf3/ai/main/event_courier.h"

#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/main/ai_main.h"

namespace bf3::ai {

void AIEventCourier::attach(AIMain *p_main, SquadUpdater *p_squads, LevelListener *p_level) {
	main_ = p_main;
	squads_ = p_squads;
	level_ = p_level;
}

void AIEventCourier::event_player_created(bool p_is_ai) {
	post({ AiEventKind::PlayerCreated, 0, 0, 0, p_is_ai });
}

void AIEventCourier::event_game_start() {
	post({ AiEventKind::GameStart });
}

void AIEventCourier::event_new_squad_leader(int p_side, int p_squad) {
	post({ AiEventKind::NewSquadLeader, p_side, p_squad });
}

void AIEventCourier::event_joined_squad(int p_player_id, int p_squad, int p_side) {
	post({ AiEventKind::JoinedSquad, p_player_id, p_squad, p_side });
}

void AIEventCourier::event_level_loaded() {
	post({ AiEventKind::LevelLoaded });
}

void AIEventCourier::event_player_switch_sides(int p_side, int p_player_id) {
	post({ AiEventKind::PlayerSwitchSides, p_side, p_player_id });
}

void AIEventCourier::post(const AiEvent &p_event) {
	queue_.push_back(p_event);
}

size_t AIEventCourier::deliver() {
	size_t delivered = 0;
	while (!queue_.empty()) {
		const AiEvent event = queue_.front();
		queue_.pop_front();
		dispatch(event);
		++delivered;
	}
	return delivered;
}

void AIEventCourier::dispatch(const AiEvent &p_event) {
	switch (p_event.kind) {
		case AiEventKind::PlayerCreated:
			if (main_ != nullptr) {
				main_->new_player_created(p_event.flag);
			}
			return;
		case AiEventKind::GameStart:
			if (main_ != nullptr) {
				main_->spawn_bots();
			}
			return;
		case AiEventKind::NewSquadLeader:
			if (squads_ != nullptr) {
				squads_->new_squad_leader(p_event.a, p_event.b);
			}
			return;
		case AiEventKind::JoinedSquad:
			if (squads_ != nullptr) {
				squads_->player_joins_squad(p_event.a, p_event.b, p_event.c);
			}
			return;
		case AiEventKind::LevelLoaded:
			if (level_ != nullptr) {
				level_->post_load_init_areas();
			}
			return;
		case AiEventKind::PlayerSwitchSides:
			if (main_ != nullptr) {
				main_->switch_sides(p_event.a, p_event.b);
			}
			return;
	}
}

}
