#pragma once
// Loading helpers shared by bf3dump, the install tests and the Godot demo (M2a audio; docs/design/m2a-audio.md §3,
// §5.3): compile a patch by name, read the SoundWaveAssets of a level or of a set of programs, and load a WaveBank of
// them. Loader thread / game thread only (they allocate and read the install through the mount).

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

// The primary SoundPatchAsset of partition p_patch (lowercase name), compiled with p_options.
core::Result<std::shared_ptr<const core::audio::SoundProgram>> compile_patch(const core::EbxResolver &p_db, std::string_view p_patch,
		const core::audio::CompileOptions &p_options = {});

// Every SoundWaveAsset partition of p_db, read (a wave that fails to read fails the call).
core::Result<std::vector<core::audio::SoundWaveInfo>> read_level_waves(const core::EbxDb &p_db);

// The distinct SoundWaveAssets the programs name (by GUID through p_db; a wave p_db lacks is skipped and counted in
// r_missing when given).
core::Result<std::vector<core::audio::SoundWaveInfo>> read_program_waves(const core::EbxResolver &p_db,
		std::span<const std::shared_ptr<const core::audio::SoundProgram>> p_programs, size_t *r_missing = nullptr);

} // namespace bf3::audio
