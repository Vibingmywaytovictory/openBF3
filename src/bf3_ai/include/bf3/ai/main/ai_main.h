#pragma once

#include "bf3/ai/knowledge/information_factory.h"
#include "bf3/ai/knowledge/weapon_info.h"
#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/main/ai_random.h"
#include "bf3/ai/main/ai_settings.h"
#include "bf3/ai/main/behaviour_manager.h"
#include "bf3/ai/main/bot_manager.h"
#include "bf3/ai/main/event_courier.h"

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

namespace bf3::ai {

struct AiMainServices {
	AiSettings *settings = nullptr;
	const AiTimer *timer = nullptr;
	const InformationManager *information = nullptr;
	const BotBehaviourManager *behaviours = nullptr;
	AiGame *game = nullptr;
	BotFactory *factory = nullptr;
	PathfindingService *pathfinding = nullptr;
	SquadUpdater *squads = nullptr;
};

class AIMain {
public:
	explicit AIMain(const AiMainServices &p_services);
	AIMain(const AIMain &) = delete;
	AIMain &operator=(const AIMain &) = delete;

	std::string init(size_t p_strategic_object_count);
	bool is_initialised() const { return initialised_; }
	bool action();
	void spawn_bots();
	bool activate_bot(int p_player_id, int p_side, const std::string &p_name);
	bool add_unspawned_bot(int p_player_id, int p_side, const float *p_personality);
	void new_player_created(bool p_is_ai);
	void switch_sides(int p_side, int p_player_id);
	void set_sai(int p_side, StrategicAi *p_sai);
	StrategicAi *sai(int p_side) const;
	bool set_sai_enable(bool p_enable, size_t p_strategic_object_count);
	bool sai_enabled() const { return sai_enabled_; }
	void queue_pending_bot(Bot *p_bot);

	BotManager &bot_manager() { return *bot_manager_; }
	const BotManager &bot_manager() const { return *bot_manager_; }
	int bot_player_count() const { return bot_count_; }
	int human_player_count() const { return human_count_; }
	uint32_t component_cursor() const { return component_cursor_; }
	double ai_system_quotient() const { return ai_system_quotient_; }
	double component_quotient() const { return component_quotient_; }

private:
	AiMainServices services_;
	double ai_system_quotient_ = 0.15f;
	double component_quotient_ = 0.125;
	bool initialised_ = false;
	bool sai_enabled_ = false;
	std::array<StrategicAi *, 3> sai_{};
	uint32_t component_cursor_ = 0;
	std::unique_ptr<BotManager> bot_manager_;
	std::list<Bot *> pending_bots_;
	int bot_count_ = 0;
	int human_count_ = 0;
};

struct AiServices {
	AiSettings settings;
	const AiTimer *timer = nullptr;
	const AltitudeSource *altitude = nullptr;
	AiGame *game = nullptr;
	BotFactory *factory = nullptr;
	PathfindingService *pathfinding = nullptr;
	SquadUpdater *squads = nullptr;
	LevelListener *level = nullptr;
	uint64_t match_seed = 0;
	int infantry_map = 0;
};

class BF2AI {
public:
	enum class Part : uint8_t {
		InformationManager,
		WeaponInfoManager,
		InformationFactory,
		Settings,
		EventCourier,
		BehaviourManager,
		Random,
		Main,
	};

	BF2AI() = default;
	~BF2AI();
	BF2AI(const BF2AI &) = delete;
	BF2AI &operator=(const BF2AI &) = delete;

	bool init(const AiServices &p_services);
	bool clear();
	bool shutdown();
	bool update();
	bool is_initialized() const { return initialized_; }
	bool is_shutting_down() const { return shutting_down_; }
	const std::vector<Part> &init_order() const { return init_order_; }
	const std::vector<Part> &clear_order() const { return clear_order_; }

	InformationManager &information() { return *information_; }
	WeaponInfoManager &weapons() { return *weapons_; }
	const InformationFactory &factory() const { return *factory_; }
	AiSettings &settings() { return *settings_; }
	AIEventCourier &events() { return *events_; }
	BotBehaviourManager &behaviours() { return *behaviours_; }
	AIRandom &random() { return *random_; }
	AIMain &main() { return *main_; }
	const KnowledgeContext &knowledge() const { return knowledge_; }

private:
	bool initialized_ = false;
	bool shutting_down_ = false;
	KnowledgeContext knowledge_;
	std::unique_ptr<InformationManager> information_;
	std::unique_ptr<WeaponInfoManager> weapons_;
	std::unique_ptr<InformationFactory> factory_;
	std::unique_ptr<AiSettings> settings_;
	std::unique_ptr<AIEventCourier> events_;
	std::unique_ptr<BotBehaviourManager> behaviours_;
	std::unique_ptr<AIRandom> random_;
	std::unique_ptr<AIMain> main_;
	std::vector<Part> init_order_;
	std::vector<Part> clear_order_;
};

}
