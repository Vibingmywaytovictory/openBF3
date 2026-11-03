#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/ids.h"
#include "bf3/core/level/area_volume.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_runtime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::sim::logic {

inline constexpr int32_t kIrReverbVolume = core::gameplay::logic_port_id("Volume");

struct IrReverbSource {
	uint32_t node = core::gameplay::kNoLogicIndex;
	int32_t port = 0;
};

struct IrReverbDef {
	uint32_t node = core::gameplay::kNoLogicIndex;
	core::Guid impulse;
	std::string impulse_name;
	float gain_db = 0.0f;
	float volume = 0.0f;
	core::level::AreaFadeCurve curve = core::level::AreaFadeCurve::SineAmplitude;
	std::vector<IrReverbSource> sources;

	float amplitude(float p_volume) const;
};

struct IrReverbValue {
	float volume = 0.0f;
	float amplitude = 0.0f;
};

struct IrReverbNativeStats {
	size_t zones = 0;
	size_t without_impulse = 0;
	size_t unfed = 0;
	uint64_t updates = 0;
};

class IrReverbNatives {
public:
	static core::Result<IrReverbNatives> create(
			const core::gameplay::LogicWorld &p_world, std::shared_ptr<const LogicGraph> p_graph);
	IrReverbNatives(IrReverbNatives &&) noexcept;
	IrReverbNatives &operator=(IrReverbNatives &&) noexcept;
	~IrReverbNatives();

	void update(const LogicRuntime &p_runtime);
	void reset();

	bool updated() const;
	std::span<const IrReverbDef> zones() const;
	std::span<const IrReverbValue> values() const;
	const IrReverbNativeStats &stats() const;

private:
	struct State;
	explicit IrReverbNatives(std::unique_ptr<State> p_state);
	std::unique_ptr<State> state_;
};

}
