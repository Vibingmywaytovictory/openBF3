#include "bf3/sim_harness/sound_area_probe.h"

#include <utility>

namespace bf3::sim::logic {

namespace gp = core::gameplay;

core::Result<SoundAreaProbe> SoundAreaProbe::create(
		const gp::LogicWorld &p_world, std::shared_ptr<const LogicGraph> p_graph) {
	core::Result<SoundAreaNatives> natives = SoundAreaNatives::create(p_world, p_graph);
	if (!natives) {
		return natives.error();
	}
	LogicRuntimeOptions o;
	o.immediate_properties = true;
	core::Result<LogicRuntime> rt = LogicRuntime::create(std::move(p_graph), o);
	if (!rt) {
		return rt.error();
	}
	SoundAreaProbe p;
	p.runtime_ = std::make_unique<LogicRuntime>(std::move(*rt));
	p.natives_ = std::make_unique<SoundAreaNatives>(std::move(*natives));
	core::Result<LogicTickStats> started = p.runtime_->start();
	if (!started) {
		return started.error();
	}
	return p;
}

core::Status SoundAreaProbe::at(const core::Vec3 &p_listener, uint32_t p_ticks) {
	for (uint32_t i = 0; i < p_ticks; ++i) {
		BF3_TRY(natives_->listen(p_listener, *runtime_));
		core::Result<LogicTickStats> stepped = runtime_->step();
		if (!stepped) {
			return stepped.error();
		}
		BF3_TRY(natives_->settle(*runtime_));
	}
	return core::Status::success();
}

}
