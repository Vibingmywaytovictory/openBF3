#pragma once
// The resident wave chunk store (M2a audio A1; docs/design/m2a-audio.md §5.3). One immutable WaveBank per loaded
// level holds the SoundWaveInfo of every wave the level's sound programs can play and, for each, the bytes of its
// chunks exactly as the install stores them decoded from their containers (XAS stays compressed: plan D15, §2.4
// "never cached: decoded audio"; gap audio §9: 0.12 ms per channel-second makes per-voice decoding cheap, PCM16
// would be 3.37x the bytes). The mixer decodes from these bytes per voice (StreamDecoder).
//
// Built on a loader thread (chunks read in parallel through bf3::core::parallel_for), published as
// std::shared_ptr<const WaveBank>; afterwards every method is const and thread-safe, nothing allocates, and the
// spans stay valid while the bank lives. The audio thread only ever reads it.
//
// Residency (A1): every non-localised wave is resident, including the StreamPool ones (Metro TDM: 629 waves,
// 249.4 MiB for the 608 without a pool + 16.0 MiB for the 21 with one [verified 2026-10-15]). Localised (VO) waves are resident only when
// WaveBankOptions::include_localised (A7 replaces this with streaming through the VO StreamPool: 491 MiB of English
// VO on Metro is too much to keep). The total is checked against resident_budget_bytes before anything is read: a
// bank over budget fails with the sizes, it is never silently truncated.
//
// Not cached (plan §2.4): the chunk bytes are re-read from the install at every level load.

#include "bf3/core/audio/sound_wave.h"
#include "bf3/core/ids.h"
#include "bf3/core/install.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::audio {

using WaveId = uint32_t; // index into WaveBank::waves()
inline constexpr WaveId kNoWave = 0xffffffffu;

struct WaveBankOptions {
	uint64_t resident_budget_bytes = uint64_t(384) << 20; // Metro TDM needs 265.4 MiB
	bool include_localised = false; // A1: VO stays out (A7 streams it)
	std::string language = "English"; // the Localization range a localised wave plays
	unsigned threads = 0; // 0 = all cores (loader-thread rule, plan §2.3)
};

struct WaveBankStats {
	size_t waves = 0;
	size_t resident_waves = 0;
	size_t chunks = 0;
	uint64_t resident_bytes = 0;
	size_t skipped_localised = 0;
	double read_seconds = 0.0;
};

class WaveBank {
public:
	// Reads every chunk of p_waves (deduplicated by GUID) from p_mount, falling back to the toc of
	// Win32/Loc/<language code> for localised waves. Fails when a needed chunk is absent, a segment offset lies
	// outside its chunk, or the total passes the budget.
	static Result<std::shared_ptr<const WaveBank>> load(const Install &p_install, const LevelMount &p_mount, std::vector<SoundWaveInfo> p_waves,
			const WaveBankOptions &p_options);

	// A bank from bytes already in memory (unit tests, tools): p_chunks[w][c] are the bytes of wave w's chunk c, each
	// as long as its ChunkSize. The same checks as load() (sizes, every playable segment's header).
	static Result<std::shared_ptr<const WaveBank>> from_memory(std::vector<SoundWaveInfo> p_waves, std::vector<std::vector<std::vector<uint8_t>>> p_chunks,
			std::string_view p_language = "English");

	const std::vector<SoundWaveInfo> &waves() const { return waves_; }
	std::optional<WaveId> find(const Guid &p_partition) const; // by the wave partition's GUID
	// The bytes of a wave's chunk; empty when that wave is not resident.
	std::span<const uint8_t> chunk(WaveId p_wave, uint16_t p_chunk_index) const;
	bool resident(WaveId p_wave) const;
	const WaveBankStats &stats() const { return stats_; }

private:
	WaveBank() = default;
	std::vector<SoundWaveInfo> waves_;
	std::vector<std::vector<uint8_t>> chunk_bytes_; // one per distinct chunk
	std::vector<std::vector<uint32_t>> wave_chunks_; // per wave: index into chunk_bytes_ per chunk ref, or ~0u
	std::unordered_map<Guid, WaveId, GuidHash> by_partition_;
	WaveBankStats stats_;
};

} // namespace bf3::core::audio
