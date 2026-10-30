#pragma once

#include "bf3/ai/bot/bot_main.h"
#include "bf3/ai/main/ai_random.h"
#include "bf3/ai/main/ai_settings.h"
#include "bf3/ai/squad/bot_squad.h"
#include "bf3/ai/squad/squad_manager.h"
#include "bf3/ai/strategic/bot_spawner.h"
#include "bf3/ai/strategic/sai.h"
#include "bf3/ai/strategic/strategy.h"
#include "bf3/core/result.h"
#include "bf3/host/ai_world.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::host {

class GameServer;
class TdmRules;
struct TdmShared;

class TdmStrategy final : public ai::AiTimer,
						  public ai::StrategyHeat,
						  public ai::SquadOrderSink,
						  public ai::SaiGame,
						  public ai::SquadStrategy {
public:
	static core::Result<std::unique_ptr<TdmStrategy>> create(GameServer &r_server, TdmShared &r_shared);
	~TdmStrategy() override;
	TdmStrategy(const TdmStrategy &) = delete;
	TdmStrategy &operator=(const TdmStrategy &) = delete;

	void add_bot(uint8_t p_slot, ai::BotMain &r_bot);
	void remove_bot(uint8_t p_slot);
	void step(sim::Tick p_tick);

	sim::Tick game_tick() const override { return tick_; }
	double tick_seconds() const override { return double(sim::kTickSeconds); }
	double update_time() const override { return double(tick_) * double(sim::kTickSeconds); }

	float area_heat(const ai::AIStrategicArea &p_area) const override;
	void squad_order(int p_side, int p_squad, ai::SquadOrder p_order, int p_area, const ai::Vec3 &p_position) override;
	int tickets(int p_side) const override;
	int number_of_control_points(int p_side) const override;
	bool is_player_commander(int p_side) const override;
	bool own_all_takeable_control_points(int p_side) const override;
	bool have_any_owned_and_takeable_control_points(int p_side) const override;

	const ai::StrategyManager &manager() const { return manager_; }
	const ai::StrategyDecisions &decisions() const { return *decisions_; }
	const ai::SquadOrders &orders() const { return *orders_; }
	const ai::SquadManager &squads() const { return *squads_; }

private:
	TdmStrategy(GameServer &r_server, TdmShared &r_shared);
	void sync_squads();
	void choose_spawns();

	GameServer &server_;
	TdmShared &shared_;
	TdmRules *rules_ = nullptr;
	std::unique_ptr<HostAiWorld> world_;
	std::unique_ptr<RulesSquadService> service_;
	ai::AiSettings settings_;
	ai::StrategyTuning tuning_;
	ai::StrategyManager manager_;
	std::unique_ptr<ai::SquadManager> squads_;
	std::array<std::unique_ptr<ai::Sai>, 2> sai_;
	std::unique_ptr<ai::StrategyDecisions> decisions_;
	std::unique_ptr<ai::SquadOrders> orders_;
	std::unique_ptr<ai::AIRandom> random_;
	std::unique_ptr<ai::BotSpawner> spawner_;
	std::array<std::unique_ptr<ai::BotSquad>, 64> bots_;
	std::array<uint32_t, 64> spawn_life_{};
	std::array<sim::Tick, 2> next_cycle_{};
	std::vector<float> heat_;
	sim::Tick heat_tick_ = UINT32_MAX;
	sim::Tick tick_ = 0;
	sim::Tick stepped_ = UINT32_MAX;
	uint64_t revision_ = UINT64_MAX;
};

}
