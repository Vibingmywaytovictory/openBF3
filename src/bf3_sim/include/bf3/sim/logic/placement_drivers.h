#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/level/census.h"
#include "bf3/sim/logic/logic_runtime.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::sim::logic {

std::vector<uint32_t> bind_placement_drivers(
		const LogicGraph &p_graph, std::span<const core::level::LogicDriverRef> p_drivers);

bool placement_driver_value(
		const LogicRuntime &p_runtime, uint32_t p_node, const core::level::LogicDriverRef &p_driver);

}
