#pragma once

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

using WaveId = uint32_t;
inline constexpr WaveId kNoWave = 0xffffffffu;

struct WaveBankOptions {
	uint64_t resident_budget_bytes = uint64_t(384) << 20;
	bool include_localised = false;
	std::string language = "English";
	unsigned threads = 0;
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

	static Result<std::shared_ptr<const WaveBank>> load(const Install &p_install, const LevelMount &p_mount, std::vector<SoundWaveInfo> p_waves,
			const WaveBankOptions &p_options);

	static Result<std::shared_ptr<const WaveBank>> from_memory(std::vector<SoundWaveInfo> p_waves, std::vector<std::vector<std::vector<uint8_t>>> p_chunks,
			std::string_view p_language = "English");

	const std::vector<SoundWaveInfo> &waves() const { return waves_; }
	std::optional<WaveId> find(const Guid &p_partition) const;

	std::span<const uint8_t> chunk(WaveId p_wave, uint16_t p_chunk_index) const;
	bool resident(WaveId p_wave) const;
	const WaveBankStats &stats() const { return stats_; }

private:
	WaveBank() = default;
	std::vector<SoundWaveInfo> waves_;
	std::vector<std::vector<uint8_t>> chunk_bytes_;
	std::vector<std::vector<uint32_t>> wave_chunks_;
	std::unordered_map<Guid, WaveId, GuidHash> by_partition_;
	WaveBankStats stats_;
};

}
