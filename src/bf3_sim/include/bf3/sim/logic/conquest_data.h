#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/conquest_rules.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::core::gameplay {
class LogicWorld;
}
namespace bf3::core::level {
class World;
}

namespace bf3::sim::logic {

inline constexpr size_t kMaxConquestNodes = 1024;

struct ConquestSpawnerData {
	uint32_t node = core::gameplay::kNoLogicIndex;
	bool vehicle = false;
	uint32_t level_object = core::gameplay::kNoLogicIndex;
	SpawnerGate gate;
};

struct ConquestPointData {
	uint32_t node = core::gameplay::kNoLogicIndex;
	CapturePointParams params;
	bool placed = false;
	std::vector<uint32_t> spawners;
};

struct ConquestCounterData {
	uint32_t node = core::gameplay::kNoLogicIndex;
	TicketCounterParams params;
};

struct ConquestAreaData {
	uint32_t node = core::gameplay::kNoLogicIndex;
	CombatAreaParams params;
	std::vector<std::array<float, 2>> polygon;
};

struct ConquestParams {
	std::vector<ConquestPointData> points;
	std::vector<ConquestCounterData> counters;
	std::vector<ConquestAreaData> areas;
	std::vector<ConquestSpawnerData> spawners;
	std::vector<std::string> notes;

	const ConquestSpawnerData *spawner(uint32_t p_node) const;
};

core::Result<std::shared_ptr<const ConquestParams>> read_conquest_params(const core::gameplay::LogicWorld &p_world,
		const core::gameplay::LogicGraph &p_graph, const core::level::World *p_level);

}
