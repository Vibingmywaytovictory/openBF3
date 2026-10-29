#pragma once

#include "bf3/audio/mixer_core.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/sound_scope.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace bf3::audio {

inline constexpr uint32_t kMaxCullVoices = 1024;

struct CullVoice {
	InstanceHandle handle = kNoInstance;
	float loudness_db = 0.0f;
	float priority = 1.0f;
	uint16_t scope = core::audio::kNoScope;
	float position[3] = { 0, 0, 0 };
	float distance_m = 0.0f;
	uint64_t start_frame = 0;
	bool looping = false;
	bool starting = false;
	bool hdr = true;
	uint32_t voices = 1;
	uint32_t held_voices = 0;
	bool parked = false;

	float hold_db = 0.0f;

	uint32_t resume_margin = 0;
};

enum class CullVerdict : uint8_t { Keep = 0, Precull = 1, Scope = 2, Cap = 3, Pending = 4 };

struct CullCounts {
	uint32_t precull = 0, scope = 0, cap = 0, pending = 0;
};

void cull_voices(std::span<const CullVoice> p_voices, const core::audio::ScopeTable *p_scopes, float p_precull_db,
		uint32_t p_max_voices, uint64_t p_frame, uint32_t p_mix_rate, std::span<CullVerdict> r_verdicts,
		CullCounts &r_counts, std::span<CullVerdict> r_park_reasons = {});

struct MixerA3Assets {
	std::optional<core::audio::HdrSetting> hdr;
	std::shared_ptr<const core::audio::ScopeTable> scopes;
	std::shared_ptr<const core::audio::MasterMixData> master;
	size_t scoped_patches = 0;
	size_t skipped_patches = 0;
	std::string notes;
};

MixerA3Assets load_mixer_a3(const core::EbxResolver &p_db, std::span<const std::shared_ptr<const core::audio::SoundProgram>> p_programs);

void apply_mixer_a3(const MixerA3Assets &p_assets, MixerConfig &r_config);

}
