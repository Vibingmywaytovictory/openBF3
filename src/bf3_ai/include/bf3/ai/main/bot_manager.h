#pragma once

#include "bf3/ai/knowledge/knowledge_context.h"
#include "bf3/ai/main/bot.h"

#include <cstdint>
#include <list>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace bf3::ai {

class AIMain;
class AiGame;
class BotBehaviourManager;
class PathfindingService;
struct AiSettings;

struct BotManagerServices {
	const AiSettings *settings = nullptr;
	const AiTimer *timer = nullptr;
	const InformationManager *information = nullptr;
	const BotBehaviourManager *behaviours = nullptr;
	AiGame *game = nullptr;
	BotFactory *factory = nullptr;
	PathfindingService *pathfinding = nullptr;
};

class BotManager {
public:
	enum class Update : uint8_t { Idle = 0, DecisionMaking = 1, Pathfinding = 2, Hearing = 3, Sensing = 4 };

	struct BotInfo {
		explicit BotInfo(Bot *p_bot) : bot(p_bot) {}
		Bot *bot = nullptr;
		int priority = 0;
		int sensing_priority = 0;
		double since_decision = 0.0;
		int behaviour_update_count = 0;
		uint32_t decision_updates = 0;
		uint32_t sensing_updates = 0;
		double decision_run_time = 0.0;
		double sensing_run_time = 0.0;
		float damage = 0.0f;
		int attacker_index = 0;
	};

	BotManager(AIMain &r_main, const BotManagerServices &p_services);
	~BotManager();
	BotManager(const BotManager &) = delete;
	BotManager &operator=(const BotManager &) = delete;

	void action(double p_budget);
	void update_damages();
	void action_com_agent();
	void action_update_nav_points();
	void action_decision_making(double p_slice);
	void action_pathfinding(double p_slice);
	void action_sensing(double p_slice);

	int bot_count() const;
	int number_of_active_bots() const;
	int number_of_deactivated_bots() const;
	Bot *bot(uint32_t p_index) const;
	Bot *bot_from_id(uint32_t p_id) const;
	Bot *bot_from_player_id(int p_player_id) const;
	Bot *last_created_bot() const;
	const BotInfo &info(uint32_t p_index) const { return *infos_[p_index]; }
	const std::list<Update> &updates() const { return updates_; }

	bool add_unspawned_bot(int p_player_id, int p_side, const float *p_personality);
	bool reactivate_bot(int p_player_id, int p_side, const std::string &p_name);
	bool request_decision(uint32_t p_index, bool p_planned, int p_priority);
	bool request_sensing(uint32_t p_index, int p_priority);

	void set_lod_level_ticks(uint32_t p_full, uint32_t p_medium, uint32_t p_light);
	void set_lod_level_priority(uint32_t p_full, uint32_t p_medium, uint32_t p_light);
	void set_planned_decision_making_threshold(double p_full, double p_medium, double p_light);
	void set_unplanned_decision_making_threshold(double p_full, double p_medium, double p_light);
	void set_decision_making_interleave(uint32_t p_planned, uint32_t p_unplanned);
	void set_sensing_quotient(uint32_t p_see, uint32_t p_sense);
	void set_system_quotient(uint32_t p_decision, uint32_t p_pathfinding, uint32_t p_sensing);

private:
	struct PriorityPred {
		bool operator()(const BotInfo *p_a, const BotInfo *p_b) const { return p_a->priority < p_b->priority; }
	};
	struct SensingPriorityPred {
		bool operator()(const BotInfo *p_a, const BotInfo *p_b) const {
			return p_a->sensing_priority < p_b->sensing_priority;
		}
	};
	using BotQueue = std::priority_queue<BotInfo *, std::vector<BotInfo *>, PriorityPred>;
	using SensingQueue = std::priority_queue<BotInfo *, std::vector<BotInfo *>, SensingPriorityPred>;

	uint32_t lod_ticks(BotLodLevel p_level) const;
	bool has_equipment(const Bot &p_bot) const;
	double now() const;

	AIMain &main_;
	BotManagerServices services_;
	BotQueue planned_queue_;
	BotQueue unplanned_queue_;
	SensingQueue sensing_queue_;
	std::list<std::unique_ptr<Bot>> free_bots_;
	std::vector<std::unique_ptr<Bot>> bots_;
	std::vector<std::unique_ptr<BotInfo>> infos_;
	std::list<uint32_t> deactivated_bots_;
	std::vector<int> bot_by_player_id_;
	uint32_t com_agent_cursor_ = 0;
	uint32_t lod_full_priority_ = 3;
	uint32_t lod_medium_priority_ = 3;
	uint32_t lod_light_priority_ = 3;
	uint32_t planned_decision_making_interleave_ = 2;
	uint32_t unplanned_decision_making_interleave_ = 2;
	double planned_threshold_full_ = 0.5;
	double planned_threshold_medium_ = 0.5;
	double planned_threshold_light_ = 0.5;
	double unplanned_threshold_full_ = 0.3;
	double unplanned_threshold_medium_ = 0.4;
	double unplanned_threshold_light_ = 0.4;
	uint32_t lod_full_ticks_ = 6;
	uint32_t lod_medium_ticks_ = 6;
	uint32_t lod_light_ticks_ = 6;
	double system_decision_making_quotient_ = 0.3;
	double system_pathfinding_quotient_ = 0.4;
	double sensing_quotient_ = 0.5;
	double sensing_quotient_inverse_ = 0.5;
	std::vector<BotInfo *> selected_bots_;
	Bot *last_created_bot_ = nullptr;
	uint32_t nav_point_cursor_ = 0;
	uint32_t pathfinding_cursor_ = 0;
	std::list<Update> updates_;
};

}
