#pragma once

#include "bf3/core/audio/sound_program.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::core::audio {

struct SoundPatchConfigAsset {
	uint32_t name_hash = 0;
	SoundWaveRef wave;
};

struct SoundPatchConfigParameter {
	uint32_t name_hash = 0;
	float value = 0.0f;
};

struct SoundPatchConfiguration {
	std::string name;
	Guid partition;
	Guid patch;
	std::string patch_name;
	float loudness_db = 0.0f;
	std::vector<SoundPatchConfigAsset> assets;
	std::vector<SoundPatchConfigParameter> parameters;
};

Result<SoundPatchConfiguration> read_sound_patch_configuration(
		const EbxResolver &p_db, const EbxPartition &p_partition);

Result<SoundProgram> configure_sound_program(SoundProgram p_patch, const SoundPatchConfiguration &p_config);

}
