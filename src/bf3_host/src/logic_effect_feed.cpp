#include "bf3/host/logic_effect_feed.h"

#include "bf3/core/gameplay/logic_types.h"

#include <algorithm>

namespace bf3::host {

namespace gp = core::gameplay;

namespace {

constexpr int32_t kStart = gp::logic_port_id("Start");
constexpr int32_t kStop = gp::logic_port_id("Stop");

}

LogicEffectFeed LogicEffectFeed::bind(const gp::LogicGraph &p_graph) {
	LogicEffectFeed out;
	const std::span<const uint32_t> effects = p_graph.nodes_of_type(gp::LogicTypeId::Effect);
	out.nodes_.assign(effects.begin(), effects.end());
	std::sort(out.nodes_.begin(), out.nodes_.end());
	out.stats_.effect_nodes = out.nodes_.size();
	return out;
}

std::vector<std::pair<uint32_t, int32_t>> LogicEffectFeed::watch_ports() const {
	std::vector<std::pair<uint32_t, int32_t>> out;
	out.reserve(nodes_.size() * 2);
	for (const uint32_t n : nodes_) {
		out.emplace_back(n, kStart);
		out.emplace_back(n, kStop);
	}
	return out;
}

bool LogicEffectFeed::event(uint32_t p_node, int32_t p_port, sim::Tick p_tick) {
	if ((p_port != kStart && p_port != kStop) || !std::binary_search(nodes_.begin(), nodes_.end(), p_node)) {
		return false;
	}
	if (events_.size() >= kLogicEffectFeedCapacity) {
		const size_t drop = kLogicEffectFeedCapacity / 2;
		events_.erase(events_.begin(), events_.begin() + ptrdiff_t(drop));
		first_ += drop;
		stats_.trimmed += drop;
	}
	events_.push_back(LogicEffectEvent{ p_node, p_port == kStart, p_tick });
	++(p_port == kStart ? stats_.starts : stats_.stops);
	return true;
}

void LogicEffectFeed::reset() {
	first_ += events_.size();
	events_.clear();
	++generation_;
}

std::span<const LogicEffectEvent> LogicEffectFeed::since(uint64_t p_cursor) const {
	const uint64_t from = std::max(p_cursor, first_);
	if (from >= end()) {
		return {};
	}
	return std::span<const LogicEffectEvent>(events_).subspan(size_t(from - first_));
}

}
