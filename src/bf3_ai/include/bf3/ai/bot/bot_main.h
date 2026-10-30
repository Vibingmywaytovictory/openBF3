#pragma once

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/behaviour/urgency_merger.h"
#include "bf3/ai/bot/infantry.h"
#include "bf3/ai/main/bot.h"
#include "bf3/ai/plan/infantry_entries.h"
#include "bf3/ai/squad/squad_world.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace bf3::ai {

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
	void set_name(const std::string &p_name) override { name_ = p_name; }
	void set_personality(const float *p_weights) override;
	void receive_idle_order(int p_idle_behaviour) override { idle_behaviour_ = p_idle_behaviour; }
	bool is_dead() const override { return deactivated_ || !alive_; }
	bool man_down() const override { return man_down_; }
	bool is_deactivated() const override { return deactivated_; }
	void set_deactivated(bool p_deactivated) override { deactivated_ = p_deactivated; }
	BotLodLevel lod_level() const override { return lod_level_; }
	uint32_t current_equipment() const override { return current_equipment_; }
	bool has_squad_member() const override { return squad_member_ != nullptr; }
	int decision_making(double p_slice) override;
	void see(double p_slice) override;
	void update_sensing(double p_slice) override;
	void update_communication() override {}
	bool path_search_pending() const override { return false; }
	void update_obstacles() override {}
	bool navigation_active() const override { return false; }
	void update_navigation_occasionally() override {}
	bool search_active() const override { return false; }
	bool search_new() const override { return false; }
	bool search_running() const override { return false; }
	void update_pathfinding(bool &r_first, bool *r_second) override;

	void set_squad_member(SquadMember *p_member) override { squad_member_ = p_member; }
	bool join_squad(int p_squad) override;
	void set_squad_goal(const SquadGoal &p_goal) override { squad_goal_ = p_goal; }

	void update_bot(bool p_alive, bool p_new_life);
	void plan_execution(double p_slice);
	bool wants_decision() const;
	void reset();

	bool receive_order(const BotOrder &p_order);
	void set_taking_orders(bool p_taking) { taking_orders_ = p_taking; }
	bool is_taking_orders() const { return taking_orders_; }
	void calculate_mods();
	bool has_plan() const { return !plan_invalid_; }
	void invalidate_plan() { plan_invalid_ = true; }
	bool plan_lock() const { return plan_lock_; }
	void set_plan_lock(bool p_lock) { plan_lock_ = p_lock; }
	void set_man_down(bool p_down) { man_down_ = p_down; }
	void set_lod_level(BotLodLevel p_level) { lod_level_ = p_level; }
	void set_current_equipment(uint32_t p_handle) { current_equipment_ = p_handle; }

	InfantryBehaviour behaviour() const;
	InfantryActivity activity() const { return plan_.activity(); }
	const BotScript &plan() const { return plan_; }
	const BotMainStats &stats() const { return stats_; }
	float behaviour_urgency(InfantryBehaviour p_behaviour) const;
	float weighted_urgency(InfantryBehaviour p_behaviour) const;
	const BotBehaviour &behaviour_of(InfantryBehaviour p_behaviour) const { return *behaviours_[size_t(p_behaviour)]; }
	float chosen_urgency() const { return chosen_urgency_; }
	const SquadGoal &squad_goal() const { return squad_goal_; }
	int squad() const { return squad_; }
	const std::string &name() const { return name_; }
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
	BotScript plan_;
	BotBehaviour *current_ = nullptr;
	float chosen_urgency_ = 0.0f;
	InfantrySenses decided_senses_;
	sim::Tick decided_tick_ = 0;
	bool plan_invalid_ = true;
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
	SquadGoal squad_goal_;
	int squad_ = 0;
	BotMainStats stats_;
};

}
