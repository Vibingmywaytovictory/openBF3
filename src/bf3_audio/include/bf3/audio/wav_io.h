#pragma once

#include "bf3/audio/mixer_core.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace bf3::audio {

enum class WavSampleFormat : uint8_t { Float32, Pcm16 };

core::Status write_wav(const core::PathGuard &p_guard, const std::filesystem::path &p_path, std::span<const float> p_interleaved,
		uint32_t p_channels, uint32_t p_rate, WavSampleFormat p_format = WavSampleFormat::Float32);

struct WavData {
	uint32_t channels = 0;
	uint32_t rate = 0;
	std::vector<float> samples;
};
core::Result<WavData> read_wav(const std::filesystem::path &p_path, uint64_t p_max_bytes = uint64_t(1) << 30);

class WavRecorder final : public MixTap {
public:
	WavRecorder(uint32_t p_rate, double p_max_seconds);
	void on_block(const float *p_interleaved_stereo, uint32_t p_frames, uint64_t p_first_frame) override;
	core::Status save(const core::PathGuard &p_guard, const std::filesystem::path &p_path) const;
	std::span<const float> samples() const;
	uint64_t dropped_frames() const;

private:
	uint32_t rate_ = 0;
	std::vector<float> buffer_;
	std::atomic<uint64_t> written_{ 0 };
	std::atomic<uint64_t> dropped_{ 0 };
};

}
