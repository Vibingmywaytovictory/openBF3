#pragma once

#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_wave.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace bf3::audio {

core::Result<std::shared_ptr<const core::audio::SoundProgram>> compile_patch(const core::EbxResolver &p_db, std::string_view p_patch,
		const core::audio::CompileOptions &p_options = {});

core::Result<std::vector<core::audio::SoundWaveInfo>> read_level_waves(const core::EbxDb &p_db);

core::Result<std::vector<core::audio::SoundWaveInfo>> read_program_waves(const core::EbxResolver &p_db,
		std::span<const std::shared_ptr<const core::audio::SoundProgram>> p_programs, size_t *r_missing = nullptr);

}
