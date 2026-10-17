#pragma once
// SoundWaveAsset reader (M2a audio A1; docs/design/m2a-audio.md §4.1, §5.2). Plain data that no longer points into
// the partition. Fields as the EBX descriptors spell them [verified 2026-10-15 with `bf3dump dump` on
// sound/weapons/shared_content/hifi_lmg/hifi_lmg_m4_wave and sound/vo/common/ru/autotriggers/vo_ru_mp_alert_hmg]:
//   SoundWaveAsset : SoundDataAsset { ChannelCount, Chunks[] {ChunkId Guid, ChunkSize}, Variations[] -> ref
//     SoundWaveVariation, Localization[] {Language -> AudioLanguage (Name, LanguageMapping), FirstVariationIndex,
//     VariationCount}, StreamPool -> StreamPoolAsset | null, Seekable, Selection (SwvsRandom ...),
//     PersistentVariationCount, PreferAvailableVariations, VoicePriority, SubtitleStringIds[], Name, NameHash }
//   SoundWaveVariation { ChunkIndex, FirstLoopSegmentIndex, LastLoopSegmentIndex, SeekTablesSize,
//     Segments[] {SamplesOffset, SegmentLength (s), SeekTableOffset} }
// A localised wave (VO) lists all languages' variations in one array; Localization[] gives each language's range.
// Its chunks live in the chunk-only superbundle Win32/Loc/<lang> (only `en` is installed), so a WaveBank reads them
// through Install::chunk_toc, not the level mount [verified: 280 Metro VO waves, every chunk found in Loc/en].
//
// Bounds (hostile input, §11): at most kMaxWaveChunks chunks, kMaxWaveVariations variations and
// kMaxVariationSegments segments per variation (retail maxima on Metro TDM: 69, 416 and 29), every ChunkIndex
// below the chunk count, every localisation range inside the variation list; all reads charged to the WorkBudget.

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
	uint32_t size = 0; // ChunkSize as listed (the bytes read_chunk returns)
};

struct WaveSegment {
	uint32_t samples_offset = 0; // byte offset of the segment's stream in the chunk
	float length_seconds = 0.0f; // equals total_frames / rate within one frame on every retail segment [verified]
};

struct WaveVariation {
	uint16_t chunk_index = 0;
	int16_t first_loop_segment = -1; // FirstLoopSegmentIndex
	int16_t last_loop_segment = -1; // LastLoopSegmentIndex
	std::vector<WaveSegment> segments;
};

struct WaveLocalization {
	std::string language; // AudioLanguage.Name, e.g. "English"
	uint32_t first_variation = 0;
	uint32_t variation_count = 0;
};

struct SoundWaveInfo {
	std::string name; // lowercase partition name
	Guid partition;
	Guid instance;
	uint8_t channels = 1; // ChannelCount
	std::vector<WaveChunkRef> chunks;
	std::vector<WaveVariation> variations;
	std::vector<WaveLocalization> localization; // empty: not localised
	std::string stream_pool; // partition of the StreamPoolAsset, empty when none
	bool seekable = false;
	std::string selection; // Selection enumerator, e.g. "SwvsRandom"
	uint8_t voice_priority = 0;

	bool localised() const { return !localization.empty(); }
};

// Reads one SoundWaveAsset instance (the primary of a wave partition).
Result<SoundWaveInfo> read_sound_wave(const EbxResolver &p_db, const EbxInstance &p_wave, gameplay::WorkBudget &r_budget);

// The variations a listener of p_language plays: [first, first + count). A wave without localisation plays all of
// them; a localised wave without p_language gives nullopt.
std::optional<std::pair<uint32_t, uint32_t>> playable_variations(const SoundWaveInfo &p_wave, std::string_view p_language);

} // namespace bf3::core::audio
