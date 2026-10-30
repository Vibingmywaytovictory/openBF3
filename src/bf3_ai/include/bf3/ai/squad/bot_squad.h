#pragma once

#include "bf3/ai/squad/squad_coordinator.h"
#include "bf3/ai/squad/squad_world.h"
#include "bf3/ai/strategic/strategy.h"

#include <array>
#include <cstdint>
#include <vector>

namespace bf3::ai {

class BotMain;
class SquadManager;

class SquadOrderSink {
public:
	virtual ~SquadOrderSink() = default;
	virtual void squad_order(int p_side, int p_squad, SquadOrder p_order, int p_area, const Vec3 &p_position) = 0;
};

class BotSquad {
public:
	BotSquad(BotMain &r_owner, SquadWorld &r_world, SquadService *p_squads, const StrategyTuning &p_tuning);
	~BotSquad();
	BotSquad(const BotSquad &) = delete;
	BotSquad &operator=(const BotSquad &) = delete;

	bool has_attack_order() const { return order_ == SquadOrder::Attack; }
	bool has_defend_order() const { return order_ == SquadOrder::Defend; }
	bool has_move_order() const { return order_ == SquadOrder::Move; }
	bool has_no_order() const { return order_ == SquadOrder::None; }
	SquadOrder order() const { return order_; }
	bool leading() const { return leading_; }

	bool join_squad(int p_squad);
	bool is_squad_leader() const;
	int squad_id() const { return squad_id_; }
	BotMain &bot() const { return *bot_; }

	void issue_squad_attack_order(const SquadCoordinator &p_squad, SquadOrderSink *p_sink);
	void issue_squad_move_order(const SquadCoordinator &p_squad, SquadOrderSink *p_sink);
	void issue_squad_defend_order(const SquadCoordinator &p_squad, SquadOrderSink *p_sink);

	void recieve_attack_squad_order();
	void recieve_defend_squad_order();
	void recieve_move_squad_order();
	void recieve_attack_commander_order(SquadCoordinator &r_squad, SquadOrderSink *p_sink);
	void recieve_defend_commander_order(SquadCoordinator &r_squad, SquadOrderSink *p_sink);
	void recieve_move_commander_order(SquadCoordinator &r_squad, SquadOrderSink *p_sink);
	void accept_squad_order(SquadCoordinator &r_squad, SquadOrderSink *p_sink);
	void decline_squad_order(SquadCoordinator &r_squad);
	void follow_squad();
	void lead_squad(SquadOrder p_order);
	void clear_order();

private:
	bool able() const;
	void issue(const SquadCoordinator &p_squad, SquadOrder p_order, SquadOrderSink *p_sink);
	void apply(SquadOrder p_order, bool p_leading, const float *p_weights);

	BotMain *bot_ = nullptr;
	SquadWorld *world_ = nullptr;
	SquadService *squads_ = nullptr;
	const StrategyTuning *tuning_ = nullptr;
	int squad_id_ = 0;
	SquadOrder order_ = SquadOrder::None;
	bool leading_ = false;
	const float *weights_ = nullptr;
	bool applied_ = false;
};

inline constexpr size_t kCohesionBins = 128;

struct SquadCohesion {
	uint64_t samples = 0;
	uint64_t near = 0;
	std::array<uint64_t, kCohesionBins> metres{};
};

struct SquadOrderStats {
	uint64_t issued = 0;
	uint64_t accepted = 0;
	uint64_t declined = 0;
	uint64_t completed = 0;
	uint64_t passed_down = 0;
};

class SquadOrders {
public:
	SquadOrders(SquadManager &r_squads, SquadOrderSink *p_sink, const BehaviourTuning &p_behaviour, float p_wait_s);
	SquadOrders(const SquadOrders &) = delete;
	SquadOrders &operator=(const SquadOrders &) = delete;

	void add_bot(int p_player_id, BotSquad &r_squad);
	void remove_bot(int p_player_id);
	void deployed(int p_player_id);
	void update(sim::Tick p_tick, float p_near_m, SquadCohesion &r_cohesion);
	BotSquad *bot_squad(int p_player_id) const;
	const SquadOrderStats &stats() const { return stats_; }

private:
	void pass_down(const SquadCoordinator &p_squad, SquadOrder p_order);
	void gather(const SquadCoordinator &p_squad) const;
	bool arrived(const SquadCoordinator &p_squad, sim::Tick p_tick, sim::Tick &r_since) const;
	bool left_origin(int p_player_id, const Vec3 &p_position, float p_metres);
	void sample(const SquadCoordinator &p_squad, float p_near_m, SquadCohesion &r_cohesion);

	SquadManager &squads_;
	SquadOrderSink *sink_ = nullptr;
	const BehaviourTuning &behaviour_;
	std::array<BotSquad *, 256> bots_{};
	std::array<Vec3, 256> origin_{};
	std::array<bool, 256> origin_known_{};
	sim::Tick wait_ticks_ = 0;
	std::array<std::vector<SquadOrder>, 2> sent_;
	std::array<std::vector<sim::Tick>, 2> arrived_;
	SquadOrderStats stats_;
};

}
