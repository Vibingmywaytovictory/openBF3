#pragma once
// Sound wave streams (M2a audio A1; docs/design/m2a-audio.md §5.1). Every SoundWaveVariationSegment of a
// SoundWaveAsset starts at its SamplesOffset in the wave's chunk with a stream of its own: a header block, data
// blocks, an end block. Blocks open with a kind byte and a big-endian 24-bit size that counts the block's 4 header
// bytes. Format and decoder are a C++ port of IW4L crates/resource_bf3/src/sound.rs (Apache-2.0, the user's own
// code), with the hostile-input bounds below added.
//
//   header block 0x48: [0x48, size24] [codec | flags] [(channels-1) << 2 | rate >> 16] [rate & 0xffff, BE]
//                      [total frames, BE u32 & 0x1fffffff] ...
//   data block   0x44: [0x44, size24] [frames in block, BE u32] body
//   end block    0x45: [0x45, size24]
// Codecs: 4 = EA-XAS v1 (frames of 128 samples, one 76-byte frame per channel in turn; a frame is 4 groups of 32
// samples with their own predictor and shift), 2 = PCM16 big endian. Retail multiplayer data uses XAS only
// [verified 2026-10-15: 12,955 Metro TDM segments and 15,890 Loc/en streams, all codec 4; rates 11,025 / 16,000 /
// 44,100 / 48,000; 1, 2, 4, 6 and 8 channels]. PCM16BE is kept because the IW4L reader supports it.
//
// Hostile input (§11): every block size is checked against the bytes left and must be >= 4 (8 for a data block),
// so a walk advances at least 4 bytes per block; channels 1..kMaxStreamChannels; rate kMinSampleRate..kMaxSampleRate;
// a data block's frame count may not exceed what its body holds; a header whose total frames exceed what the bytes
// after it can hold is refused before anything is allocated; decoding never produces more than total_frames.

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
inline constexpr uint32_t kMaxStreamChannels = 8; // retail maximum (Loc/en cinematics)
inline constexpr uint32_t kMinSampleRate = 1000;
inline constexpr uint32_t kMaxSampleRate = 192000;
inline constexpr uint32_t kMaxStreamFrames = 0x1fffffff; // the header field's mask

struct SoundStreamHeader {
	SoundCodec codec = SoundCodec::EaXas;
	uint8_t channels = 1;
	uint32_t sample_rate = 48000;
	uint32_t total_frames = 0;
	uint32_t header_bytes = 0; // size of the header block (the first data block follows)
};

// The header block at p_offset of a chunk. Fails on a short chunk, another block kind, an unknown codec, a channel
// count or rate out of range, or total frames the remaining bytes cannot hold.
Result<SoundStreamHeader> parse_stream_header(std::span<const uint8_t> p_chunk, size_t p_offset);

// What a full walk of one stream found (census, gates G1/G2).
struct StreamScan {
	SoundStreamHeader header;
	uint32_t decoded_frames = 0; // frames the data blocks yield, capped at total_frames
	uint32_t data_blocks = 0;
	uint64_t stream_bytes = 0; // header block .. end block inclusive (0x45 consumed when present)
	bool ended_by_end_block = false;
};
// Walks (p_decode: and decodes, discarding the samples) the stream at p_offset. O(stream bytes).
Result<StreamScan> scan_stream(std::span<const uint8_t> p_chunk, size_t p_offset, bool p_decode);

// The whole stream as interleaved int16 frames (tests, the analysis tools; never on the audio thread).
struct DecodedStream {
	SoundStreamHeader header;
	std::vector<int16_t> samples; // frames * channels, interleaved
	uint32_t frames() const { return header.channels ? uint32_t(samples.size() / header.channels) : 0; }
};
Result<DecodedStream> decode_stream(std::span<const uint8_t> p_chunk, size_t p_offset);

// Decodes one 76-byte XAS frame into 128 samples (exposed for unit vectors).
void decode_xas_frame(std::span<const uint8_t, kXasFrameBytes> p_frame, std::array<int16_t, kXasFrameSamples> &r_out);

// Incremental decoder of one stream: the mixer's sampler voice (audio thread). No allocation after open(); the
// chunk bytes must outlive the decoder (they are owned by the WaveBank, §5.3). Decodes one XAS frame (128 samples
// per channel) at a time into an internal buffer and hands out frames as asked. Trivially copyable, so a voice
// pool can hold decoders inline.
class StreamDecoder {
public:
	StreamDecoder() = default;

	// Parses the header at p_offset and positions at the first data block.
	Status open(std::span<const uint8_t> p_chunk, size_t p_offset);
	const SoundStreamHeader &header() const { return header_; }
	uint32_t position() const { return position_; } // frames handed out so far
	bool finished() const { return position_ >= header_.total_frames; }

	// Writes up to p_frames frames (channels() floats each, scaled to [-1, 1)) to r_out; returns the frames
	// written: fewer than asked only at the end of the stream. Fails on malformed data (the voice then stops).
	Result<uint32_t> read(float *r_out, uint32_t p_frames);
	// Skips to frame p_frame (forward only; Sampler `Offset`); decodes the frame it lands in.
	Status skip_to(uint32_t p_frame);

private:
	const uint8_t *chunk_ = nullptr;
	size_t chunk_size_ = 0;
	SoundStreamHeader header_{};
	size_t cursor_ = 0; // next block or frame byte
	size_t block_end_ = 0; // end of the current data block's body
	uint32_t block_frames_left_ = 0; // frames the current data block still declares
	uint32_t position_ = 0;
	uint16_t buffered_ = 0; // frames left in buffer_
	uint16_t buffered_at_ = 0; // next frame index in buffer_
	std::array<int16_t, kXasFrameSamples * kMaxStreamChannels> buffer_{}; // one decoded frame, interleaved
};

} // namespace bf3::core::audio
