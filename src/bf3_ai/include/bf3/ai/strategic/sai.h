#pragma once

#include "bf3/ai/main/ai_game.h"
#include "bf3/ai/squad/squad_world.h"
#include "bf3/ai/strategic/strategic_object.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace bf3::ai {

class AIInformationGrid;
class AiTimer;
class Bot;
class InformationManager;
class SquadManager;
struct AiSettings;

inline constexpr int kSaiStateCount = 44;

class SaiGame {
public:
	virtual ~SaiGame() = default;
	virtual int tickets(int p_side) const = 0;
	virtual int number_of_control_points(int p_side) const = 0;
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

class Sai final : public StrategicAi, public SquadStrategy {
public:
	struct BotInfo {
		Bot *bot = nullptr;
		float time_alive = 0.0f;
	};

	Sai(int p_side, const SaiServices &p_services);

	void add_bot(Bot *p_bot) override;
	void remove_bot(Bot *p_bot) override;
	void update(double p_budget) override;

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
	void set_player_is_commander(bool p_commander) { player_commander_ = p_commander; }

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
	const std::vector<BotInfo> &bot_info() const { return bot_info_; }
	bool strategic_objects_changed() const { return objects_changed_; }
	uint32_t completed_updates() const { return completed_; }
	const SaiServices &services() const { return services_; }

private:
	static void clear_states(std::array<float, kSaiStateCount> &r_own, std::array<float, kSaiStateCount> &r_enemy);
	void update_strengths();
	void update_states(std::array<float, kSaiStateCount> &r_own, std::array<float, kSaiStateCount> &r_enemy);
	void prepare_update();
	bool update_strategic_objects();
	double game_time() const;
	bool over_budget(double p_start, double p_budget) const;

	SaiServices services_;
	int side_ = 1;
	int side_mask_ = 0;
	int enemy_mask_ = 0;
	SaiStage stage_ = SaiStage::Done;
	double last_update_ = -992.0;
	bool player_commander_ = false;
	std::vector<BotInfo> bot_info_;
	std::vector<Bot *> added_;
	std::vector<Bot *> removed_;
	std::map<int, std::pair<int, int>> dead_bots_;
	std::map<int, int> spawns_to_points_;
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
