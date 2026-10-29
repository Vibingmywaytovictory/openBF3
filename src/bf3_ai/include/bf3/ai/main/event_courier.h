#pragma once

#include <cstdint>
#include <deque>

namespace bf3::ai {

class AIMain;
class LevelListener;
class SquadUpdater;

enum class AiEventKind : uint8_t {
	PlayerCreated,
	GameStart,
	NewSquadLeader,
	JoinedSquad,
	LevelLoaded,
	PlayerSwitchSides,
};

struct AiEvent {
	AiEventKind kind = AiEventKind::GameStart;
	int a = 0;
	int b = 0;
	int c = 0;
	bool flag = false;
};

class AIEventCourier {
public:
	void attach(AIMain *p_main, SquadUpdater *p_squads, LevelListener *p_level);

	void event_player_created(bool p_is_ai);
	void event_game_start();
	void event_new_squad_leader(int p_side, int p_squad);
	void event_joined_squad(int p_player_id, int p_squad, int p_side);
	void event_level_loaded();
	void event_player_switch_sides(int p_side, int p_player_id);

	void post(const AiEvent &p_event);
	size_t deliver();
	size_t pending() const { return queue_.size(); }

private:
	void dispatch(const AiEvent &p_event);

	AIMain *main_ = nullptr;
	SquadUpdater *squads_ = nullptr;
	LevelListener *level_ = nullptr;
	std::deque<AiEvent> queue_;
};

}
