#pragma once
// Voice culling (docs/design/m2b.md §6.2, §2.6 #15; declared in W1.0, owner au). Audio thread, inside MixerCore, once
// per block; allocation-free (the caller owns every buffer; MixerCore sizes them to its slot count at construction).
//
// The order (research gap-r1-audio-system.md §11.3): (1) pre-cull a new one-shot below the previous block's window
// bottom - 3 dB (HdrWindow::precull_db); (2) scopes: duplicates within the radius and time, then the closest N, then
// the max count (sound_scope.h); (3) over the voice cap (MixerConfig::max_voices, 128), drop the lowest loudness x
// priority: loops become pending (resume when a voice frees), one-shots are dropped. Every cull is counted per step
// (MixerStats::culled_precull, culled_scope, culled_cap).
//
// As built (au, docs/design/m2b-au.md "Culling"):
//   - a voice is "starting" in the block its Start lands and while it is pending (a pending loop starts again when it
//     is kept); pre-cull and DuplicateSpawn look only at starting voices, so a playing voice that falls below the
//     bottom is suspended by the window instead (HdrWindow::suspended), never culled by step 1;
//   - pre-cull applies to HDR voices only (CullVoice::hdr; MixerCore passes -inf without an HdrSetting); a starting
//     loop below the edge is Pending (counted pending, not precull);
//   - DuplicateSpawn: a starting voice within duplicate_radius_m of an earlier-kept voice of the same scope that started
//     less than duplicate_time_s before it is dropped (Scope); the earlier voice (lower start frame, then lower index)
//     wins between two starting ones;
//   - closest N among the scope's kept voices by distance (ties: the older, then the lower index); latest N: the N
//     newest stay; max_instances N: the N oldest stay (a new one is refused);
//   - a scope or cap cull of a loop is Pending, of a one-shot Scope / Cap (dropped);
//   - the cap counts sampler voices, not patch instances: CullVoice::voices is the instance's busy sampler slots
//     (playing or armed) at the block's start, at least 1 (a voice in BF3's 128 is one playing sound; a weapon patch
//     instance holds 1-8 of them, a tail 1-2). The kept voices in score order fill the cap; the first that does not
//     fit and every voice after it are culled. MixerStats::live_voices_max is that count (measured at each block's
//     start; slots a block triggers count from the next block);
//   - over the cap the score is loudness_db x priority (both positive in the data: 35..135 dB, priority 1), the
//     lowest go first (ties: the newer, then the higher index); every pass is O(n^2) over the voices of one scope or
//     of the kept set, n <= kMaxCullVoices (a larger span is refused: every verdict Keep, nothing counted).

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

inline constexpr uint32_t kMaxCullVoices = 1024; // a span above it is not culled (MixerCore passes its slot count)

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
	bool hdr = true; // as built (au): an HDR voice (an output with EnableHdr): pre-cull applies
	uint32_t voices = 1; // as built (au): its sampler voices against the cap (0 counts as 1, above kMaxCullVoices as that)
	// PT1 snd [hold] (docs/design/pt1-snd.md): added to loudness_db in the cap's ranking only (not pre-cull, not the
	// scopes): MixerCore gives a voice that sounded in the last block MixerConfig::pt1_cap_hold_db, so a parked loop
	// takes its place only when it is louder by more than that (without it, voices of equal loudness at the cap's edge
	// swapped every few blocks: docs/design/pt1-snd.md §2).
	float hold_db = 0.0f; // PT1 snd [hold]
	// PT1 snd [hold]: a parked voice comes back only when this many sampler voices stay free after it (at most
	// kMaxCullVoices), so the voice that just came back is not parked again by the next small rise in the others' busy
	// slots. Counts in its own fit test only, not against the others.
	uint32_t resume_margin = 0; // PT1 snd [hold]
};

enum class CullVerdict : uint8_t { Keep = 0, Precull = 1, Scope = 2, Cap = 3, Pending = 4 };

struct CullCounts {
	uint32_t precull = 0, scope = 0, cap = 0, pending = 0;
};

// Writes one verdict per voice into r_verdicts (same size as p_voices) and adds to r_counts.
void cull_voices(std::span<const CullVoice> p_voices, const core::audio::ScopeTable *p_scopes, float p_precull_db, uint32_t p_max_voices,
		uint64_t p_frame, uint32_t p_mix_rate, std::span<CullVerdict> r_verdicts, CullCounts &r_counts);

// As built (au): the A3 inputs of a game mixer, read on the loader thread with the audio bank (m2b.md §7): the
// install's HdrSetting and its scopes with the patch -> scope map of the level's compiled programs.
struct MixerA3Assets {
	std::optional<core::audio::HdrSetting> hdr; // nullopt: the read failed (`notes` says why), the fixed window stays
	std::shared_ptr<const core::audio::ScopeTable> scopes; // null: the read failed
	size_t scoped_patches = 0; // programs whose patch names a scope
	size_t skipped_patches = 0; // map_patch_scopes' skipped count
	std::string notes; // one line per failure ("" = both read)
};
// Never fails: a part that does not read is left out and noted (the mixer then runs without it).
MixerA3Assets load_mixer_a3(const core::EbxResolver &p_db, std::span<const std::shared_ptr<const core::audio::SoundProgram>> p_programs);
// Sets MixerConfig::hdr and ::scopes from p_assets (max_voices keeps its value).
void apply_mixer_a3(const MixerA3Assets &p_assets, MixerConfig &r_config);

} // namespace bf3::audio
