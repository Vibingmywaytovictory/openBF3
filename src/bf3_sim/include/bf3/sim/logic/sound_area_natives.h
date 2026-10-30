#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/gameplay/sound_area.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_runtime.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace bf3::sim::logic {

using core::audio::kMixerAreaType;

struct SoundAreaValue {
	uint32_t node = core::gameplay::kNoLogicIndex;
	float proximity = 0.0f;
	float multiplier = 1.0f;
	bool published = false;
};

struct SoundAreaNativeStats {
	size_t areas = 0;
	size_t mixers = 0;
	size_t mixer_entities = 0;
	size_t unshaped = 0;
	uint64_t listens = 0;
	uint64_t settles = 0;
	uint64_t area_publishes = 0;
	uint64_t mixer_publishes = 0;
};

class SoundAreaNatives {
public:
	static core::Result<SoundAreaNatives> create(
			const core::gameplay::LogicWorld &p_world, std::shared_ptr<const LogicGraph> p_graph);
	SoundAreaNatives(SoundAreaNatives &&) noexcept;
	SoundAreaNatives &operator=(SoundAreaNatives &&) noexcept;
	~SoundAreaNatives();

	core::Status listen(const core::Vec3 &p_listener, LogicRuntime &r_runtime);
	core::Status settle(LogicRuntime &r_runtime);
	void reset();

	bool listening() const;
	core::Vec3 listener() const;
	std::optional<float> mixer_value(uint32_t p_output_hash) const;
	float area_type() const;
	std::span<const SoundAreaValue> areas() const;
	std::span<const float> mixer_inputs(size_t p_mixer) const;
	std::span<const float> mixer_outputs(size_t p_mixer) const;
	const core::gameplay::SoundAreaData &data() const;
	const SoundAreaNativeStats &stats() const;

private:
	struct State;
	explicit SoundAreaNatives(std::unique_ptr<State> p_state);
	std::unique_ptr<State> state_;
};

}
