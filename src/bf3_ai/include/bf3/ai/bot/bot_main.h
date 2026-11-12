#pragma once

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/behaviour/urgency_merger.h"
#include "bf3/ai/bot/bot_memory.h"
#include "bf3/ai/bot/communication_agent.h"
#include "bf3/ai/bot/infantry.h"
#include "bf3/ai/interpreter/bot_plan.h"
#include "bf3/ai/main/bot.h"
#include "bf3/ai/squad/squad_world.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace bf3::ai {

inline constexpr float kQuotientLow = 0.87f;
inline constexpr float kQuotientHigh = 1.15f;

class BotOrder {
public:
	BotOrder(const float *p_weights, uint8_t p_behaviour) : weights(p_weights), behaviour(p_behaviour) {}

	const float *weights = nullptr;
	uint8_t behaviour = 0;
};

struct BotMainStats {
	uint64_t decisions = 0;
	uint64_t planned = 0;
	uint64_t unplanned = 0;
	uint64_t refusals = 0;
	uint64_t plan_failures = 0;
	std::array<uint64_t, kInfantryBehaviourCount> chosen{};
};

class BotMain final : public Bot, public SquadBot {
public:
	BotMain(uint32_t p_bot_id, int p_player_id, int p_side, InfantryBody &r_body, const BehaviourTuning &p_tuning,
			const float *p_personality = nullptr);
	~BotMain() override;
	BotMain(const BotMain &) = delete;
	BotMain &operator=(const BotMain &) = delete;

	uint32_t bot_id() const override { return bot_id_; }
	int side() const override { return side_; }
	void set_side(int p_side) override { side_ = p_side; }
	int player_id() const override { return player_id_; }
	void set_player(int p_player_id) override { player_id_ = p_player_id; }
	const std::string &name() const override { return name_; }
	void set_name(const std::string &p_name) override { name_ = p_name; }
	void set_personality(const float *p_weights) override;
	void receive_idle_order(int p_idle_behaviour) override { idle_behaviour_ = p_idle_behaviour; }
	bool is_dead() const override { return deactivated_ || !alive_; }
	bool man_down() const override { return man_down_; }
	void set_man_down(bool p_down) override { man_down_ = p_down; }
	bool is_deactivated() const override { return deactivated_; }
	void set_deactivated(bool p_deactivated) override { deactivated_ = p_deactivated; }
	bool in_transition_phase() const override { return in_transition_phase_; }
	void set_in_transition_phase(bool p_in) override { in_transition_phase_ = p_in; }
	BotLodLevel lod_level() const override { return lod_level_; }
	uint32_t current_equipment() const override { return current_equipment_; }
	void set_current_equipment(uint32_t p_handle) override { current_equipment_ = p_handle; }
	bool has_squad_member() const override { return squad_member_ != nullptr || !takes_squads_; }
	int decision_making(double p_slice) override;
	void plan_execution(double p_slice) override;
	bool has_plan() const override { return !plan_invalid_; }
	bool has_default_plan() const override { return has_default_plan_; }
	void invalidate_plan() override { plan_invalid_ = true; }
	void update_player_action(bool p_stalled) override;
	void rearm(const Information &p_equipment, const WeaponInfoManager &p_weapons, double p_time_increase) override;
	void set_bot_stuck(bool p_stuck) override { bot_stuck_ = p_stuck; }
	void see(double p_slice) override;
	void update_sensing(double p_slice) override;
	void heard_object(uint32_t) override {}
	void update_communication() override;
	CommunicationAgent *communication() const override { return communication_.get(); }
	bool path_search_pending() const override { return false; }
	void update_obstacles() override {}
	bool navigation_active() const override { return false; }
	void update_navigation_occasionally() override {}
	void reset_navigation(bool) override {}
	bool search_active() const override { return false; }
	bool search_new() const override { return false; }
	bool search_running() const override { return false; }
	void update_pathfinding(bool &r_first, bool *r_second) override;
	bool update_bot_vehicle() override;
	bool update_bot() override;
	void reset() override;
	void reset_assigned_areas() override;
	void set_last_change_time(double p_time) override { last_change_time_ = p_time; }
	void set_spawn_coord(const Vec3 &p_position) override { spawn_coord_ = p_position; }
	bool c4_placed() const override { return c4_placed_; }
	void remove_c4() override { c4_placed_ = false; }
	uint32_t mine_target() const override { return mine_target_; }
	void set_mine_target(uint32_t p_handle) override { mine_target_ = p_handle; }
	void set_is_repaired(bool p_repaired) override { is_repaired_ = p_repaired; }
	BotCallBack *death_call_back() const override { return death_call_back_.get(); }
	BotCallBack *spawn_call_back() const override { return spawn_call_back_.get(); }
	void set_death_call_back(std::unique_ptr<BotCallBack> p_call_back) { death_call_back_ = std::move(p_call_back); }
	void set_spawn_call_back(std::unique_ptr<BotCallBack> p_call_back) { spawn_call_back_ = std::move(p_call_back); }

	void set_squad_member(SquadMember *p_member) override { squad_member_ = p_member; }
	bool join_squad(int p_squad) override;
	void set_squad_goal(const SquadGoal &p_goal) override { squad_goal_ = p_goal; }

	void attach_communication(const RadioVoices &p_voices, RadioComm &r_hub);
	void call_for_medic();
	void update_bot(bool p_alive, bool p_new_life);
	void set_takes_squads(bool p_takes) { takes_squads_ = p_takes; }

	bool receive_order(const BotOrder &p_order);
	void set_taking_orders(bool p_taking) { taking_orders_ = p_taking; }
	bool is_taking_orders() const { return taking_orders_; }
	void calculate_mods();
	bool plan_lock() const { return plan_lock_; }
	void set_plan_lock(bool p_lock) { plan_lock_ = p_lock; }
	void set_lod_level(BotLodLevel p_level) { lod_level_ = p_level; }

	InfantryBehaviour behaviour() const;
	InfantryActivity activity() const { return plan_.activity(); }
	const BotPlan &plan() const { return plan_; }
	const BotMainStats &stats() const { return stats_; }
	float weighted_urgency(InfantryBehaviour p_behaviour) const;
	const BotBehaviour &behaviour_of(InfantryBehaviour p_behaviour) const { return *behaviours_[size_t(p_behaviour)]; }
	float chosen_urgency() const { return chosen_urgency_; }
	const SquadGoal &squad_goal() const { return squad_goal_; }
	const BotMemory &memory() const { return memory_; }
	int squad() const { return squad_; }
	bool bot_stuck() const { return bot_stuck_; }
	uint32_t stalled_plan_ticks() const { return stalled_plan_ticks_; }
	double last_change_time() const { return last_change_time_; }
	const Vec3 &spawn_coord() const { return spawn_coord_; }
	bool is_repaired() const { return is_repaired_; }
	const std::array<int, 4> &assigned_areas() const { return assigned_areas_; }
	int ordered_behaviour() const { return ordered_behaviour_; }

private:
	uint32_t bot_id_ = 0;
	int player_id_ = -1;
	int side_ = 0;
	std::string name_;
	InfantryBody &body_;
	const BehaviourTuning &tuning_;
	std::array<std::unique_ptr<UCLinear>, kInfantryBehaviourCount> curves_;
	std::array<std::unique_ptr<BotBehaviour>, kInfantryBehaviourCount> behaviours_;
	std::array<float, kInfantryBehaviourCount> basic_weights_{};
	std::array<float, kInfantryBehaviourCount> personality_{};
	const float *current_orders_ = nullptr;
	std::array<float, kInfantryBehaviourCount> behaviour_urgencies_{};
	std::unique_ptr<UrgencyMerger> merger_;
	BotMemory memory_;
	std::unique_ptr<CommunicationAgent> communication_;
	BotPlan plan_;
	BotBehaviour *current_ = nullptr;
	float chosen_urgency_ = 0.0f;
	InfantrySenses decided_senses_;
	bool plan_invalid_ = true;
	bool has_default_plan_ = false;
	bool in_transition_phase_ = false;
	bool bot_stuck_ = false;
	bool c4_placed_ = false;
	bool is_repaired_ = false;
	uint32_t mine_target_ = kNoHandle;
	uint32_t stalled_plan_ticks_ = 0;
	double rearm_timer_ = 0.0;
	double last_change_time_ = 0.0;
	Vec3 spawn_coord_{};
	std::array<int, 4> assigned_areas_{ -1, -1, -1, -1 };
	std::unique_ptr<BotCallBack> death_call_back_;
	std::unique_ptr<BotCallBack> spawn_call_back_;
	bool plan_lock_ = false;
	bool taking_orders_ = false;
	int ordered_behaviour_ = -1;
	int idle_behaviour_ = -1;
	bool alive_ = false;
	bool man_down_ = false;
	bool deactivated_ = false;
	BotLodLevel lod_level_ = BotLodLevel::Full;
	uint32_t current_equipment_ = UINT32_MAX;
	SquadMember *squad_member_ = nullptr;
	bool takes_squads_ = true;
	SquadGoal squad_goal_;
	int squad_ = 0;
	BotMainStats stats_;
};

}
