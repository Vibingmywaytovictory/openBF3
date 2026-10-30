#pragma once

#include "bf3/audio/mixer_core.h"
#include "bf3/sim/logic/sound_area_natives.h"

#include <cstdint>

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
	const AreaFeedStats &stats() const { return stats_; }

private:
	AreaFeedStats stats_;
};

}
