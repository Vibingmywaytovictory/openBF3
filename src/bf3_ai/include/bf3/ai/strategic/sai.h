#pragma once

#include "bf3/ai/commander/commander.h"
#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/squad/squad_world.h"
#include "bf3/ai/strategic/strategic_object.h"

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bf3::ai {

class AIInformationGrid;
class AIRandom;
class AiTimer;
class Bot;
class IPIUnit;
class InformationManager;
class SquadManager;
class Strategy;
class StrategyManager;
struct AiSettings;

inline constexpr int kSaiStateCount = 44;
inline constexpr int kUnknownBotPosition = -22;
inline constexpr int kUnknownBotOrderedPosition = -44;
inline constexpr int kDeadBotKit = 13;

class SaiGame {
public:
	virtual ~SaiGame() = default;
	virtual int tickets(int p_side) const = 0;
	virtual int number_of_control_points(int p_side) const = 0;
	virtual int spawn_kit(int p_player_id) const = 0;
};

class SaiDecisions {
public:
	virtual ~SaiDecisions() = default;
	virtual void choose_strategy(Sai &r_sai) = 0;
	virtual void modify_strategic_objects_temperatures(Sai &r_sai) = 0;
	virtual void update_bot_positions(Sai &r_sai) = 0;
	virtual void update_distribution(Sai &r_sai) = 0;
	virtual void distribute_resources(Sai &r_sai) = 0;
	virtual void distribute_defence(Sai &r_sai) = 0;
	virtual void distribute_attack(Sai &r_sai) = 0;
	virtual void distribute_with_no_areas(Sai &r_sai) = 0;
	virtual int release_squads_from_object(Sai &r_sai, int p_object, int p_side) = 0;
};

struct SaiServices {
	const AiTimer *timer = nullptr;
	const AiSettings *settings = nullptr;
	const SaiGame *game = nullptr;
	const AIStrategicObjectManager *objects = nullptr;
	const InformationManager *information = nullptr;
	const AIInformationGrid *grid = nullptr;
	SquadManager *squads = nullptr;
	SaiDecisions *decisions = nullptr;
	const StrategyManager *strategies = nullptr;
	AIRandom *random = nullptr;
	Commander *commander = nullptr;
};

enum class SaiStage : uint8_t {
	Start = 0,
	Strengths = 1,
	ClearStates = 2,
	UpdateStates = 3,
	Prepare = 4,
	StrategicObjects = 6,
	ChooseStrategy = 7,
	Temperatures = 8,
	BotPositions = 9,
	Distribution = 10,
	Resources = 11,
	Defence = 12,
	Attack = 13,
	Artillery = 14,
	NoAreas = 15,
	Done = 19,
};

enum class SaiDenial : int { Supply = 0, Artillery = 1, ArtilleryUnavailable = 2, Uav = 3, VehicleDrop = 4 };

class Sai final : public StrategicAi, public SquadStrategy {
public:
	struct BotInfo {
		explicit BotInfo(Bot *p_bot) : bot(p_bot) {}
		Bot *bot = nullptr;
		int ordered = -1;
		int last_ordered = -1;
		int occupied = -1;
		float time_alive = 0.0f;
		double order_time = 0.0;
	};

	struct StrategyContainer {
		float distributed_score = 0.0f;
		float score = 0.0f;
		float start_time = 0.0f;
		int rounds = 1;
		const Strategy *strategy = nullptr;
	};

	struct DenyRequest {
		SaiDenial denial = SaiDenial::Supply;
		double deadline = 0.0;
		int player_id = -1;
	};

	Sai(int p_side, const SaiServices &p_services);
	~Sai() override;

	void add_bot(Bot *p_bot) override;
	void remove_bot(Bot *p_bot) override;
	void update(double p_budget) override;

	void set_bot_is_dead(int p_bot_id);
	void set_bot_is_alive(int p_bot_id);
	void set_bot_has_changed_equipment(int p_bot_id) override;
	bool release_unit_from_object(int p_object, std::set<int>::iterator &r_it);
	int assign_bot(std::set<int>::iterator &r_it, int p_object, int p_bot_id, bool p_attacker);
	bool is_bot_free(int p_bot_id) const { return free_bots_.contains(p_bot_id); }
	bool is_bot_assigned(int p_bot_id) const { return assigned_bots_.contains(p_bot_id); }
	int bot_current_pos(int p_bot_id) const;
	int bot_ordered_pos(int p_bot_id) const;
	int bot_last_ordered_pos(int p_bot_id) const;
	const BotInfo *bot_info(int p_bot_id) const;

	void add_strategy(const Strategy *p_strategy);
	void choose_strategy();
	void change_strategy(size_t p_index);
	void modify_strategic_objects_temperatures();
	void update_distribution();
	void distribute_resources();
	void distribute_defence();
	void distribute_attack();
	void distribute_with_no_areas();
	int attack_resources() const { return attack_resources_; }
	int defence_resources() const { return defence_resources_; }
	int idle_resources() const { return idle_resources_; }
	bool frees_attackers() const { return free_attackers_; }
	bool frees_defenders() const { return free_defenders_; }
	const Strategy *active_strategy() const;
	const std::vector<StrategyContainer> &strategies() const { return strategies_; }
	Vec2 strategy_heat(size_t p_index) const;
	std::string list_strategies() const;

	bool player_requests_artillery(int p_player_id, const Vec3 &p_position) override;
	bool player_requests_uav(int p_player_id, const Vec3 &p_position) override;
	bool player_requests_supply(int p_player_id, const Vec3 &p_position) override;
	bool player_requests_vehicle_drop(int p_player_id, const Vec3 &p_position) override;
	void update_request_deny_timers();
	const std::list<DenyRequest> &deny_requests() const { return deny_requests_; }
	bool hostile_takeover_in_progress(AIStrategicObject *&r_base, Vec2 &r_position) const;
	bool is_pos_inside_owned_control_point(
			AIStrategicObject *&r_object, const Vec3 &p_position, float p_max_radius) const;
	int release_squads_from_object(int p_object);
	CommanderInterface commander_interface() const { return CommanderInterface(services_.commander); }

	bool is_player_commander(int p_side) const override;
	bool own_all_takeable_control_points(int p_side) const override;
	bool have_any_owned_and_takeable_control_points(int p_side) const override;
	bool have_any_owned_control_points(int p_side) const;

	bool is_enemy(int p_side) const { return (uint32_t(enemy_mask_) & (1u << (p_side & 31))) != 0; }
	bool is_friend(int p_side) const { return (uint32_t(side_mask_) & (1u << (p_side & 31))) != 0; }
	void set_new_bot_kit(int p_bot_id, int p_kit);
	const std::map<int, std::pair<int, int>> &dead_bots() const { return dead_bots_; }
	void add_spawn_to_point(int p_point);
	int spawns_to_point(int p_point) const;
	void set_player_is_commander(bool p_commander) override;

	int side() const { return side_; }
	SaiStage stage() const { return stage_; }
	const std::array<float, kSaiStateCount> &own_state() const { return state_own_[current_]; }
	const std::array<float, kSaiStateCount> &enemy_state() const { return state_enemy_[current_]; }
	const std::array<float, 6> &enemy_strengths() const { return enemy_strengths_; }
	const std::array<float, 6> &enemy_counts() const { return enemy_counts_; }
	const std::array<float, 6> &friendly_strengths() const { return friendly_strengths_; }
	const std::array<float, 6> &friendly_counts() const { return friendly_counts_; }
	const EnemyStatistics &enemy_statistics() const { return enemy_stats_; }
	const EnemyStatistics &friendly_statistics() const { return friendly_stats_; }
	bool strategic_objects_changed() const { return objects_changed_; }
	uint32_t completed_updates() const { return completed_; }
	const SaiServices &services() const { return services_; }

private:
	static void clear_states(std::array<float, kSaiStateCount> &r_own, std::array<float, kSaiStateCount> &r_enemy);
	void update_strengths();
	void update_states(std::array<float, kSaiStateCount> &r_own, std::array<float, kSaiStateCount> &r_enemy);
	void prepare_update();
	bool update_strategic_objects();
	bool over_budget(double p_start, double p_budget) const;
	float draw() const;
	void deny(SaiDenial p_denial, int p_player_id, double p_now);
	AIStrategicObject *object(int p_index) const;
	AIStrategicObject *closest_object_of(const BotInfo &p_info) const;
	void handle_dead_bot(int p_bot_id);
	void handle_changing_bot(int p_bot_id);

	void remove(BotInfo &r_info);
	void remove_assigned(BotInfo &r_info, AIStrategicObject &r_object, bool p_release);
	void remove_assigned(BotInfo &r_info, AIStrategicObject &r_object, std::set<int>::iterator &r_it, bool p_release);
	void occupy(BotInfo &r_info, AIStrategicObject &r_from, AIStrategicObject &r_to);
	void assign_to_object(BotInfo &r_info, std::set<int>::iterator &r_it, AIStrategicObject &r_object, bool p_attacker);
	void free_from(BotInfo &r_info, AIStrategicObject &r_object, bool p_attacker, std::set<int>::iterator &r_it);
	void free_bot(BotInfo &r_info, bool p_release);
	void respawn(BotInfo &r_info, AIStrategicObject &r_object);
	static void set_as_dead(BotInfo &r_info);
	const IPIUnit *current_unit(int p_bot_id) const;
	void assign_free(BotInfo &r_info, std::set<int>::iterator &r_it, bool p_attacker);
	void category_distribute_defence(const std::vector<AIStrategicObject *> &p_objects);
	void category_distribute_attack(const std::vector<AIStrategicObject *> &p_objects);
	void category_distribute_with_no_areas(const std::vector<AIStrategicObject *> &p_objects);
	void feed_least_supplied(std::vector<AIStrategicObject *> p_objects);

	SaiServices services_;
	int side_ = 1;
	int side_mask_ = 0;
	int enemy_mask_ = 0;
	SaiStage stage_ = SaiStage::Done;
	double last_update_ = -992.0;
	bool player_commander_ = false;
	std::vector<std::unique_ptr<BotInfo>> bots_;
	std::vector<Bot *> added_;
	std::set<int> free_bots_;
	std::set<int> assigned_bots_;
	std::map<int, std::pair<int, int>> dead_bots_;
	std::map<int, int> spawns_to_points_;
	std::vector<StrategyContainer> strategies_;
	size_t active_ = 0;
	std::list<DenyRequest> deny_requests_;
	int attack_resources_ = 0;
	int defence_resources_ = 0;
	int idle_resources_ = 0;
	bool free_attackers_ = false;
	bool free_defenders_ = false;
	bool released_ = true;
	double artillery_ready_ = 0.0;
	double supply_ready_ = 0.0;
	double vehicle_drop_ready_ = 0.0;
	double uav_ready_ = 0.0;
	std::array<std::array<float, kSaiStateCount>, 2> state_own_{};
	std::array<std::array<float, kSaiStateCount>, 2> state_enemy_{};
	size_t current_ = 0;
	EnemyStatistics enemy_stats_{ 6 };
	EnemyStatistics friendly_stats_{ 6 };
	std::array<float, 6> enemy_strengths_{};
	std::array<float, 6> enemy_counts_{};
	std::array<float, 6> friendly_strengths_{};
	std::array<float, 6> friendly_counts_{};
	size_t object_cursor_ = 0;
	bool objects_pass_start_ = true;
	bool objects_changed_ = false;
	uint32_t completed_ = 0;
};

}
