#pragma once

#include "bf3/core/result.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/game_server.h"

#include <memory>
#include <vector>

namespace bf3::host {

class BotDirector {
public:
	static core::Result<std::unique_ptr<BotDirector>> create(std::shared_ptr<const BotFillConfig> p_config, uint64_t p_seed);
	~BotDirector();
	BotDirector(const BotDirector &) = delete;
	BotDirector &operator=(const BotDirector &) = delete;

	uint32_t count() const;

	std::unique_ptr<CommandSource> source(uint32_t p_index);
	sim::Command command(uint32_t p_index, sim::Tick p_tick, const sim::SoldierState &p_state);
	BotFillStats stats() const;

	double paths_ms_total() const;
	const sim::bots::WalkerBrain &brain(uint32_t p_index) const;

private:
	BotDirector() = default;
	std::shared_ptr<const BotFillConfig> config_;
	std::unique_ptr<sim::NavQuery> query_;
	std::unique_ptr<sim::bots::PathService> paths_;
	std::vector<sim::bots::WalkerBrain> brains_;
	sim::Tick run_tick_ = UINT32_MAX;
	uint32_t thinks_tick_ = 0;
	uint64_t thinks_before_ = 0;
	double brains_ms_ = 0.0;
	double paths_ms_ = 0.0;
};

}
