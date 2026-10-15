#pragma once
// bf3_host internal (spike S1; docs/design/s1.md §8): the BotDirector of one GameServer. It owns the server thread's
// NavQuery and PathService and every bot's WalkerBrain; each bot slot's CommandSource forwards to it. The first
// command() call of a tick runs PathService::run, so GameServer's tick needs no new step. Server thread only.

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
	// The CommandSource of bot p_index (< count()); the director must outlive it.
	std::unique_ptr<CommandSource> source(uint32_t p_index);
	sim::Command command(uint32_t p_index, sim::Tick p_tick, const sim::SoldierState &p_state);
	BotFillStats stats() const;
	const sim::bots::WalkerBrain &brain(uint32_t p_index) const;

private:
	BotDirector() = default;
	std::shared_ptr<const BotFillConfig> config_;
	std::unique_ptr<sim::NavQuery> query_;
	std::unique_ptr<sim::bots::PathService> paths_;
	std::vector<sim::bots::WalkerBrain> brains_;
	sim::Tick run_tick_ = UINT32_MAX; // the tick PathService::run last ran for
	uint32_t thinks_tick_ = 0;
	uint64_t thinks_before_ = 0;
	double brains_ms_ = 0.0; // of run_tick_
	double paths_ms_ = 0.0;
};

} // namespace bf3::host
