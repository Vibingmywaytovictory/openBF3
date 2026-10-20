#pragma once

#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::core::audio {

enum class SoundCodec : uint8_t {
	Pcm16Be = 2,
	EaXas = 4,
};

inline constexpr uint8_t kStreamBlockHeader = 0x48;
inline constexpr uint8_t kStreamBlockData = 0x44;
inline constexpr uint8_t kStreamBlockEnd = 0x45;
inline constexpr size_t kXasFrameBytes = 76;
inline constexpr uint32_t kXasFrameSamples = 128;
inline constexpr uint32_t kXasGroupSamples = 32;
inline constexpr uint32_t kMaxStreamChannels = 8;
inline constexpr uint32_t kMinSampleRate = 1000;
inline constexpr uint32_t kMaxSampleRate = 192000;
inline constexpr uint32_t kMaxStreamFrames = 0x1fffffff;

struct SoundStreamHeader {
	SoundCodec codec = SoundCodec::EaXas;
	uint8_t channels = 1;
	uint32_t sample_rate = 48000;
	uint32_t total_frames = 0;
	uint32_t header_bytes = 0;
};

Result<SoundStreamHeader> parse_stream_header(std::span<const uint8_t> p_chunk, size_t p_offset);

struct StreamScan {
	SoundStreamHeader header;
	uint32_t decoded_frames = 0;
	uint32_t data_blocks = 0;
	uint64_t stream_bytes = 0;
	bool ended_by_end_block = false;
};

Result<StreamScan> scan_stream(std::span<const uint8_t> p_chunk, size_t p_offset, bool p_decode);

struct DecodedStream {
	SoundStreamHeader header;
	std::vector<int16_t> samples;
	uint32_t frames() const { return header.channels ? uint32_t(samples.size() / header.channels) : 0; }
};
Result<DecodedStream> decode_stream(std::span<const uint8_t> p_chunk, size_t p_offset);

void decode_xas_frame(std::span<const uint8_t, kXasFrameBytes> p_frame, std::array<int16_t, kXasFrameSamples> &r_out);

class StreamDecoder {
public:
	StreamDecoder() = default;

	Status open(std::span<const uint8_t> p_chunk, size_t p_offset);
	const SoundStreamHeader &header() const { return header_; }
	uint32_t position() const { return position_; }
	bool finished() const { return position_ >= header_.total_frames; }

	Result<uint32_t> read(float *r_out, uint32_t p_frames);

	Status skip_to(uint32_t p_frame);

private:
	const uint8_t *chunk_ = nullptr;
	size_t chunk_size_ = 0;
	SoundStreamHeader header_{};
	size_t cursor_ = 0;
	size_t block_end_ = 0;
	uint32_t block_frames_left_ = 0;
	uint32_t position_ = 0;
	uint16_t buffered_ = 0;
	uint16_t buffered_at_ = 0;
	std::array<int16_t, kXasFrameSamples * kMaxStreamChannels> buffer_{};
};

}
