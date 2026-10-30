#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/logic/sound_area_natives.h"

#include <cstdint>
#include <memory>

namespace bf3::sim::logic {

inline constexpr uint32_t kSoundAreaProbeTicks = 8;

class SoundAreaProbe {
public:
	static core::Result<SoundAreaProbe> create(
			const core::gameplay::LogicWorld &p_world, std::shared_ptr<const LogicGraph> p_graph);

	core::Status at(const core::Vec3 &p_listener, uint32_t p_ticks = kSoundAreaProbeTicks);

	const SoundAreaNatives &natives() const { return *natives_; }
	const LogicRuntime &runtime() const { return *runtime_; }

private:
	std::unique_ptr<LogicRuntime> runtime_;
	std::unique_ptr<SoundAreaNatives> natives_;
};

}
