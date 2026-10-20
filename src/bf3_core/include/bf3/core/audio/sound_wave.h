#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::audio {

inline constexpr size_t kMaxWaveChunks = 1024;
inline constexpr size_t kMaxWaveVariations = 65536;
inline constexpr size_t kMaxVariationSegments = 256;

struct WaveChunkRef {
	Guid id;
	uint32_t size = 0;
};

struct WaveSegment {
	uint32_t samples_offset = 0;
	float length_seconds = 0.0f;
};

struct WaveVariation {
	uint16_t chunk_index = 0;
	int16_t first_loop_segment = -1;
	int16_t last_loop_segment = -1;
	std::vector<WaveSegment> segments;
};

struct WaveLocalization {
	std::string language;
	uint32_t first_variation = 0;
	uint32_t variation_count = 0;
};

struct SoundWaveInfo {
	std::string name;
	Guid partition;
	Guid instance;
	uint8_t channels = 1;
	std::vector<WaveChunkRef> chunks;
	std::vector<WaveVariation> variations;
	std::vector<WaveLocalization> localization;
	std::string stream_pool;
	bool seekable = false;
	std::string selection;
	uint8_t voice_priority = 0;

	bool localised() const { return !localization.empty(); }
};

Result<SoundWaveInfo> read_sound_wave(const EbxResolver &p_db, const EbxInstance &p_wave, gameplay::WorkBudget &r_budget);

std::optional<std::pair<uint32_t, uint32_t>> playable_variations(const SoundWaveInfo &p_wave, std::string_view p_language);

}
