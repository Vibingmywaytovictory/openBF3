#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/sim/sim_types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace bf3::host {

inline constexpr size_t kLogicEffectFeedCapacity = 4096;

struct LogicEffectEvent {
	uint32_t node = 0;
	bool start = false;
	sim::Tick tick = 0;
};

struct LogicEffectFeedStats {
	size_t effect_nodes = 0;
	uint64_t starts = 0;
	uint64_t stops = 0;
	uint64_t trimmed = 0;
};

class LogicEffectFeed {
public:
	static LogicEffectFeed bind(const core::gameplay::LogicGraph &p_graph);

	std::vector<std::pair<uint32_t, int32_t>> watch_ports() const;
	bool event(uint32_t p_node, int32_t p_port, sim::Tick p_tick);
	void reset();

	uint64_t generation() const { return generation_; }
	uint64_t end() const { return first_ + events_.size(); }
	std::span<const LogicEffectEvent> since(uint64_t p_cursor) const;
	const LogicEffectFeedStats &stats() const { return stats_; }

private:
	std::vector<uint32_t> nodes_;
	std::vector<LogicEffectEvent> events_;
	uint64_t first_ = 0;
	uint64_t generation_ = 0;
	LogicEffectFeedStats stats_;
};

}
