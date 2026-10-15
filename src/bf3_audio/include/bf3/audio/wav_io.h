#pragma once
// WAV files for recordings and analysis (M2a audio; docs/design/m2a-audio.md §9). Derived audio is never committed
// (plan §2.4): recordings go to b/shots/audio/ (gitignored) or the scratch directory, and every write goes through
// bf3::core::PathGuard (a path inside the install, or not allowed, fails before anything is opened).
//
// Writes RIFF/WAVE, format 3 (IEEE float 32) or 1 (PCM 16), any channel count <= 8. The reader accepts exactly
// what the writer produces (it is for our own files; bounded by the file size, it still never trusts a chunk size).

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
	std::vector<float> samples; // interleaved
};
core::Result<WavData> read_wav(const std::filesystem::path &p_path, uint64_t p_max_bytes = uint64_t(1) << 30);

// A MixTap that copies blocks into a preallocated buffer of p_max_seconds (audio thread: memcpy only; past the end
// it counts dropped frames), written out by save() on the game thread after the audio thread stopped using it.
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

} // namespace bf3::audio
