#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kAreaObserverBlocks = 96;

core::Result<std::vector<std::string>> observe_area_waves(std::shared_ptr<const core::audio::SoundProgram> p_program,
		std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, float p_area_type,
		uint32_t p_event);

}
