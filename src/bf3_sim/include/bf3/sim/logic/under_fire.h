#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/level/area_volume.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_runtime.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::level {
class World;
}

namespace bf3::sim::logic {

// INFERRED: the unnamed output -1580778154 is the trigger's fire event (what the retail graphs link from it).
// Settle: that port's name.
inline constexpr int32_t kUnderFireOut = -1580778154;
inline constexpr size_t kMaxUnderFireTriggers = 1024;

struct UnderFireTriggerData {
	uint32_t node = core::gameplay::kNoLogicIndex;
	uint32_t level_object = core::gameplay::kNoLogicIndex;
	core::Vec3 position{};
	std::optional<core::level::AreaVolume> volume;
	float radius = 0.0f;
	float cool_down_s = 0.0f;
	float delay_s = 0.0f;
	bool run_once = false;
	bool enabled = true;

	float distance(const core::Vec3 &p_point) const;
};

struct UnderFireParams {
	std::vector<UnderFireTriggerData> triggers;
	std::vector<std::string> notes;
};

core::Result<std::shared_ptr<const UnderFireParams>> read_under_fire_params(
		const core::gameplay::LogicGraph &p_graph, const core::level::World &p_level);

struct UnderFireStats {
	uint64_t impacts = 0;
	uint64_t inside = 0;
	uint64_t fired = 0;
	uint64_t cooling = 0;
	uint64_t disabled = 0;
	uint64_t spent = 0;
};

class UnderFireNatives {
public:
	explicit UnderFireNatives(std::shared_ptr<const UnderFireParams> p_params);

	void apply(Tick p_tick, std::span<const LogicEffect> p_effects);
	core::Status impacts(Tick p_tick, std::span<const core::Vec3> p_points, LogicRuntime &r_runtime);
	void reset();

	bool enabled(uint32_t p_node) const;
	const UnderFireParams &params() const { return *params_; }
	const UnderFireStats &stats() const { return stats_; }

private:
	struct Trigger {
		bool enabled = true;
		bool spent = false;
		bool fired_once = false;
		Tick ready = 0;
		std::vector<Tick> pending;
	};
	size_t index_of(uint32_t p_node) const;

	std::shared_ptr<const UnderFireParams> params_;
	std::vector<Trigger> triggers_;
	UnderFireStats stats_;
};

}
