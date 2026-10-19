#pragma once
// Voice culling (docs/design/m2b.md §6.2, §2.6 #15; declared in W1.0, owner au). Audio thread, inside MixerCore, once
// per block; allocation-free (the caller owns every buffer, sized at construction to MixerConfig::max_voices).
//
// The order (research gap-r1-audio-system.md §11.3): (1) pre-cull a new one-shot below the previous block's window
// bottom - 3 dB (HdrWindow::precull_db); (2) scopes: duplicates within the radius and time, then the closest N, then
// the max count (sound_scope.h); (3) over the voice cap (MixerConfig::max_voices, 128), drop the lowest loudness x
// priority: loops become pending (resume when a voice frees), one-shots are dropped. Every cull is counted per step
// (MixerStats::culled_precull, culled_scope, culled_cap).

#include "bf3/audio/mixer_core.h"
#include "bf3/core/audio/sound_scope.h"

#include <cstdint>
#include <span>

namespace bf3::audio {

// One live or starting voice as the cull sees it.
struct CullVoice {
	InstanceHandle handle = kNoInstance;
	float loudness_db = 0.0f; // at the listener
	float priority = 1.0f;
	uint16_t scope = core::audio::kNoScope;
	float position[3] = { 0, 0, 0 };
	float distance_m = 0.0f; // to the listener
	uint64_t start_frame = 0; // MixerClock frame it started (or will start)
	bool looping = false;
	bool starting = false; // started this block (pre-cull and DuplicateSpawn apply to it)
};

enum class CullVerdict : uint8_t { Keep = 0, Precull = 1, Scope = 2, Cap = 3, Pending = 4 };

struct CullCounts {
	uint32_t precull = 0, scope = 0, cap = 0, pending = 0;
};

// Writes one verdict per voice into r_verdicts (same size as p_voices) and adds to r_counts.
void cull_voices(std::span<const CullVoice> p_voices, const core::audio::ScopeTable *p_scopes, float p_precull_db, uint32_t p_max_voices,
		uint64_t p_frame, uint32_t p_mix_rate, std::span<CullVerdict> r_verdicts, CullCounts &r_counts);

} // namespace bf3::audio
