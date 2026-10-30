#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/sound_area_natives.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::audio {

struct AreaFeedStats {
	uint64_t steps = 0;
	uint64_t posts = 0;
	uint64_t push_failures = 0;
	bool posted = false;
	float area_type = 0.0f;
};

class AreaFeed {
public:
	void step(MixerCore &r_mixer, const sim::logic::SoundAreaNatives *p_areas);
	void reset();
	const AreaFeedStats &stats() const { return stats_; }

private:
	AreaFeedStats stats_;
};

inline constexpr uint32_t kAreaObserverBlocks = 96;

core::Result<std::vector<std::string>> observe_area_waves(std::shared_ptr<const core::audio::SoundProgram> p_program,
		std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, float p_area_type,
		uint32_t p_event);

}
