#pragma once

#include "bf3/render/level_scene.h"
#include "bf3/sim/logic/logic_runtime.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::godot_side {

class LevelUpload;

struct LogicPlacementsStats {
	size_t drivers = 0;
	size_t bound = 0;
	size_t instances = 0;
	size_t shown = 0;
	uint64_t changes = 0;
};

class LogicPlacements {
public:
	bool frame(const render::LevelScenePlan &p_plan, const sim::logic::LogicRuntime &p_runtime, LevelUpload &r_upload);
	const LogicPlacementsStats &stats() const { return stats_; }
	std::string describe() const;

private:
	void bind(const render::LevelScenePlan &p_plan, const core::gameplay::LogicGraph &p_graph);

	const render::LevelScenePlan *plan_ = nullptr;
	const core::gameplay::LogicGraph *graph_ = nullptr;
	std::vector<uint32_t> nodes_;
	std::vector<uint8_t> values_;
	std::vector<std::vector<uint32_t>> instances_of_set_;
	LogicPlacementsStats stats_;
};

}
