// Sound wave streams: EA-XAS v1 and PCM16BE (sound_stream.h). A C++ port of IW4L crates/resource_bf3/src/sound.rs
// (Apache-2.0, the user's own code: decode_stream, block_size, decode_xas_block, decode_xas_frame), with the
// hostile-input bounds of docs/design/m2a-audio.md §11 added and an incremental decoder for the mixer's voices.
//
// Every walk advances at least 4 bytes per block and 76 bytes per XAS frame, so all work is linear in the stream's
// bytes; nothing is sized by a field before that field is checked against the bytes that would hold it.

#include "bf3/core/audio/sound_stream.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace bf3::core::audio {

namespace {

constexpr int32_t kXasCoefficients[4][2] = { { 0, 0 }, { 240, 0 }, { 460, -208 }, { 392, -220 } };
constexpr size_t kBlockHeaderBytes = 4;
constexpr size_t kDataHeaderBytes = 8; // block header + frames in block

uint32_t be24(const uint8_t *p_at) {
	return (uint32_t(p_at[0]) << 16) | (uint32_t(p_at[1]) << 8) | uint32_t(p_at[2]);
}
uint32_t be32(const uint8_t *p_at) {
	return (uint32_t(p_at[0]) << 24) | (uint32_t(p_at[1]) << 16) | (uint32_t(p_at[2]) << 8) | uint32_t(p_at[3]);
}

Error stream_error(size_t p_at, const std::string &p_what) {
	return make_error("sound stream at byte " + std::to_string(p_at) + ": " + p_what);
}

size_t bytes_per_frame_group(const SoundStreamHeader &p_header) {
	// XAS: one 76-byte frame per channel holds 128 frames; PCM16: 2 bytes per channel per frame.
	return p_header.codec == SoundCodec::EaXas ? kXasFrameBytes * p_header.channels : size_t(2) * p_header.channels;
}
uint32_t frames_per_group(const SoundStreamHeader &p_header) {
	return p_header.codec == SoundCodec::EaXas ? kXasFrameSamples : 1u;
}

// The block at p_at: kind and size, size checked against the chunk (>= 4, and >= 8 for a data block).
struct Block {
	uint8_t kind = 0;
	size_t size = 0;
};
Result<Block> read_block(std::span<const uint8_t> p_chunk, size_t p_at) {
	if (p_at > p_chunk.size() || p_chunk.size() - p_at < kBlockHeaderBytes) {
		return stream_error(p_at, "block header past the end of the chunk");
	}
	Block b;
	b.kind = p_chunk[p_at];
	b.size = be24(&p_chunk[p_at + 1]);
	if (b.size < kBlockHeaderBytes) {
		return stream_error(p_at, "block size " + std::to_string(b.size) + " < 4");
	}
	if (b.size > p_chunk.size() - p_at) {
		return stream_error(p_at, "block size " + std::to_string(b.size) + " past the end of the chunk (" + std::to_string(p_chunk.size() - p_at) +
				" bytes left)");
	}
	if (b.kind == kStreamBlockData && b.size < kDataHeaderBytes) {
		return stream_error(p_at, "data block size " + std::to_string(b.size) + " < 8");
	}
	return b;
}

// A data block's declared frames, checked against what its body holds.
Result<uint32_t> data_block_frames(std::span<const uint8_t> p_chunk, size_t p_at, const Block &p_block, const SoundStreamHeader &p_header) {
	const uint32_t frames = be32(&p_chunk[p_at + kBlockHeaderBytes]);
	const size_t body = p_block.size - kDataHeaderBytes;
	const uint64_t capacity = uint64_t(body / bytes_per_frame_group(p_header)) * frames_per_group(p_header);
	if (frames > capacity) {
		return stream_error(p_at, "data block declares " + std::to_string(frames) + " frames, its body holds " + std::to_string(capacity));
	}
	return frames;
}

} // namespace

void decode_xas_frame(std::span<const uint8_t, kXasFrameBytes> p_frame, std::array<int16_t, kXasFrameSamples> &r_out) {
	for (size_t group = 0; group < 4; ++group) {
		const uint32_t header = uint32_t(p_frame[group * 4]) | (uint32_t(p_frame[group * 4 + 1]) << 8) | (uint32_t(p_frame[group * 4 + 2]) << 16) |
				(uint32_t(p_frame[group * 4 + 3]) << 24);
		const int32_t coef1 = kXasCoefficients[header & 3][0];
		const int32_t coef2 = kXasCoefficients[header & 3][1];
		int32_t hist2 = int32_t(int16_t(uint16_t(header & 0xfff0)));
		int32_t hist1 = int32_t(int16_t(uint16_t((header >> 16) & 0xfff0)));
		const int32_t shift = 20 - int32_t((header >> 16) & 0x0f);
		const size_t base = group * kXasGroupSamples;
		r_out[base] = int16_t(hist2);
		r_out[base + 1] = int16_t(hist1);
		for (size_t row = 0; row < 15; ++row) {
			const uint8_t byte = p_frame[16 + row * 4 + group];
			const uint8_t nibbles[2] = { uint8_t(byte >> 4), uint8_t(byte & 0x0f) };
			for (size_t half = 0; half < 2; ++half) {
				const int32_t nibble = int32_t(int8_t(uint8_t(nibbles[half] << 4))) >> 4;
				int32_t sample = ((nibble * (int32_t(1) << shift)) + hist1 * coef1 + hist2 * coef2 + 128) >> 8;
				sample = std::clamp(sample, int32_t(-32768), int32_t(32767));
				r_out[base + 2 + row * 2 + half] = int16_t(sample);
				hist2 = hist1;
				hist1 = sample;
			}
		}
	}
}

Result<SoundStreamHeader> parse_stream_header(std::span<const uint8_t> p_chunk, size_t p_offset) {
	Result<Block> block = read_block(p_chunk, p_offset);
	if (!block) {
		return block.error();
	}
	if (block->kind != kStreamBlockHeader) {
		return stream_error(p_offset, "expected a header block (0x48), found 0x" + std::to_string(block->kind));
	}
	if (block->size < kBlockHeaderBytes + 8) {
		return stream_error(p_offset, "header block size " + std::to_string(block->size) + " < 12");
	}
	const uint8_t *h = &p_chunk[p_offset + kBlockHeaderBytes];
	SoundStreamHeader out;
	const uint8_t codec = h[0] & 0x0f;
	if (codec != uint8_t(SoundCodec::EaXas) && codec != uint8_t(SoundCodec::Pcm16Be)) {
		return stream_error(p_offset, "unsupported codec " + std::to_string(codec));
	}
	out.codec = SoundCodec(codec);
	out.channels = uint8_t((h[1] >> 2) + 1);
	if (out.channels < 1 || out.channels > kMaxStreamChannels) {
		return stream_error(p_offset, std::to_string(out.channels) + " channels (1.." + std::to_string(kMaxStreamChannels) + ")");
	}
	out.sample_rate = (uint32_t(h[1] & 0x03) << 16) | (uint32_t(h[2]) << 8) | uint32_t(h[3]);
	if (out.sample_rate < kMinSampleRate || out.sample_rate > kMaxSampleRate) {
		return stream_error(p_offset, "sample rate " + std::to_string(out.sample_rate) + " out of range");
	}
	out.total_frames = be32(h + 4) & kMaxStreamFrames;
	out.header_bytes = uint32_t(block->size);
	// What the bytes after the header could hold at most (block headers ignored): refuse before anything is sized.
	const size_t after = p_chunk.size() - p_offset - block->size;
	const uint64_t most = uint64_t(after / bytes_per_frame_group(out)) * frames_per_group(out);
	if (out.total_frames > most) {
		return stream_error(p_offset, std::to_string(out.total_frames) + " frames declared, the " + std::to_string(after) + " bytes after the header hold at most " +
				std::to_string(most));
	}
	return out;
}

namespace {

// The shared block walk: calls p_frames(at, block frames) for each data block's body (at = first body byte) until
// total_frames are reached or the end block is met.
template <class OnData>
Result<StreamScan> walk_stream(std::span<const uint8_t> p_chunk, size_t p_offset, OnData &&p_on_data) {
	Result<SoundStreamHeader> header = parse_stream_header(p_chunk, p_offset);
	if (!header) {
		return header.error();
	}
	StreamScan scan;
	scan.header = *header;
	size_t cursor = p_offset + header->header_bytes;
	uint64_t produced = 0;
	while (produced < header->total_frames) {
		if (cursor == p_chunk.size()) {
			break; // the chunk ends without an end block: a short stream (counted by the caller)
		}
		Result<Block> block = read_block(p_chunk, cursor);
		if (!block) {
			return block.error();
		}
		if (block->kind == kStreamBlockEnd) {
			cursor += block->size;
			scan.ended_by_end_block = true;
			break;
		}
		if (block->kind != kStreamBlockData) {
			return stream_error(cursor, "unexpected block kind 0x" + std::to_string(block->kind));
		}
		Result<uint32_t> frames = data_block_frames(p_chunk, cursor, *block, *header);
		if (!frames) {
			return frames.error();
		}
		const uint32_t take = uint32_t(std::min<uint64_t>(*frames, header->total_frames - produced));
		p_on_data(cursor + kDataHeaderBytes, take);
		produced += take;
		++scan.data_blocks;
		cursor += block->size;
	}
	// The end block that follows the last data block.
	if (!scan.ended_by_end_block && cursor < p_chunk.size() && p_chunk.size() - cursor >= kBlockHeaderBytes && p_chunk[cursor] == kStreamBlockEnd) {
		const size_t size = be24(&p_chunk[cursor + 1]);
		if (size >= kBlockHeaderBytes && size <= p_chunk.size() - cursor) {
			cursor += size;
			scan.ended_by_end_block = true;
		}
	}
	scan.decoded_frames = uint32_t(produced);
	scan.stream_bytes = cursor - p_offset;
	return scan;
}

} // namespace

Result<StreamScan> scan_stream(std::span<const uint8_t> p_chunk, size_t p_offset, bool p_decode) {
	Result<SoundStreamHeader> parsed = parse_stream_header(p_chunk, p_offset);
	if (!parsed) {
		return parsed.error();
	}
	const SoundStreamHeader header = *parsed;
	volatile int32_t sink = 0; // keeps the decode from being optimised away
	int32_t acc = 0;
	std::array<int16_t, kXasFrameSamples> frame{};
	auto on_data = [&](size_t p_body, uint32_t p_frames) {
		if (!p_decode) {
			return;
		}
		if (header.codec == SoundCodec::EaXas) {
			const size_t groups = (size_t(p_frames) + kXasFrameSamples - 1) / kXasFrameSamples;
			for (size_t g = 0; g < groups; ++g) {
				for (size_t ch = 0; ch < header.channels; ++ch) {
					const uint8_t *at = &p_chunk[p_body + (g * header.channels + ch) * kXasFrameBytes];
					decode_xas_frame(std::span<const uint8_t, kXasFrameBytes>(at, kXasFrameBytes), frame);
					acc += frame[5];
				}
			}
		} else {
			for (size_t i = 0; i < size_t(p_frames) * header.channels; ++i) {
				acc += int16_t(uint16_t((p_chunk[p_body + i * 2] << 8) | p_chunk[p_body + i * 2 + 1]));
			}
		}
	};
	Result<StreamScan> out = walk_stream(p_chunk, p_offset, on_data);
	sink = acc;
	(void)sink;
	return out;
}

Result<DecodedStream> decode_stream(std::span<const uint8_t> p_chunk, size_t p_offset) {
	Result<SoundStreamHeader> header = parse_stream_header(p_chunk, p_offset);
	if (!header) {
		return header.error();
	}
	DecodedStream out;
	out.header = *header;
	const size_t lanes = header->channels;
	// Sized once (bounded by the bytes: parse_stream_header), written in place, trimmed to what the blocks gave.
	out.samples.resize(size_t(header->total_frames) * lanes);
	int16_t *dst = out.samples.data();
	size_t written = 0;
	std::array<int16_t, kXasFrameSamples> decoded{};
	auto on_data = [&](size_t p_body, uint32_t p_frames) {
		if (header->codec == SoundCodec::EaXas) {
			uint32_t left = p_frames;
			size_t g = 0;
			while (left > 0) {
				const uint32_t n = std::min(left, kXasFrameSamples);
				for (size_t ch = 0; ch < lanes; ++ch) {
					const uint8_t *at = &p_chunk[p_body + (g * lanes + ch) * kXasFrameBytes];
					decode_xas_frame(std::span<const uint8_t, kXasFrameBytes>(at, kXasFrameBytes), decoded);
					int16_t *lane = dst + written + ch;
					for (uint32_t i = 0; i < n; ++i) {
						lane[i * lanes] = decoded[i];
					}
				}
				written += size_t(n) * lanes;
				left -= n;
				++g;
			}
		} else {
			for (size_t i = 0; i < size_t(p_frames) * lanes; ++i) {
				dst[written + i] = int16_t(uint16_t((p_chunk[p_body + i * 2] << 8) | p_chunk[p_body + i * 2 + 1]));
			}
			written += size_t(p_frames) * lanes;
		}
	};
	Result<StreamScan> scan = walk_stream(p_chunk, p_offset, on_data);
	if (!scan) {
		return scan.error();
	}
	out.samples.resize(written);
	return out;
}

// ---- StreamDecoder --------------------------------------------------------------------------------------------

Status StreamDecoder::open(std::span<const uint8_t> p_chunk, size_t p_offset) {
	*this = StreamDecoder();
	Result<SoundStreamHeader> header = parse_stream_header(p_chunk, p_offset);
	if (!header) {
		return header.error();
	}
	chunk_ = p_chunk.data();
	chunk_size_ = p_chunk.size();
	header_ = *header;
	cursor_ = p_offset + header->header_bytes;
	block_end_ = cursor_;
	return Status::success();
}

Result<uint32_t> StreamDecoder::read(float *r_out, uint32_t p_frames) {
	uint32_t written = 0;
	const std::span<const uint8_t> chunk(chunk_, chunk_size_);
	const size_t lanes = header_.channels;
	while (written < p_frames && position_ < header_.total_frames) {
		if (buffered_ == 0) {
			// Move to the next data block when the current one has no frames left.
			while (block_frames_left_ == 0) {
				cursor_ = block_end_;
				if (cursor_ == chunk_size_) {
					return stream_error(cursor_, "stream ends at frame " + std::to_string(position_) + " of " + std::to_string(header_.total_frames));
				}
				Result<Block> block = read_block(chunk, cursor_);
				if (!block) {
					return block.error();
				}
				if (block->kind != kStreamBlockData) {
					return stream_error(cursor_, block->kind == kStreamBlockEnd ? "end block at frame " + std::to_string(position_) + " of " +
											std::to_string(header_.total_frames)
																			: "unexpected block kind");
				}
				Result<uint32_t> frames = data_block_frames(chunk, cursor_, *block, header_);
				if (!frames) {
					return frames.error();
				}
				block_end_ = cursor_ + block->size;
				cursor_ += kDataHeaderBytes;
				block_frames_left_ = *frames;
			}
			const uint32_t n = std::min(block_frames_left_, kXasFrameSamples); // PCM16 is buffered 128 frames at a time too
			if (header_.codec == SoundCodec::EaXas) {
				std::array<int16_t, kXasFrameSamples> one{};
				for (size_t ch = 0; ch < lanes; ++ch) {
					decode_xas_frame(std::span<const uint8_t, kXasFrameBytes>(chunk_ + cursor_ + ch * kXasFrameBytes, kXasFrameBytes), one);
					for (uint32_t i = 0; i < n; ++i) {
						buffer_[i * lanes + ch] = one[i];
					}
				}
				cursor_ += kXasFrameBytes * lanes;
			} else {
				for (size_t i = 0; i < size_t(n) * lanes; ++i) {
					buffer_[i] = int16_t(uint16_t((chunk_[cursor_ + i * 2] << 8) | chunk_[cursor_ + i * 2 + 1]));
				}
				cursor_ += size_t(n) * lanes * 2;
			}
			block_frames_left_ -= n;
			buffered_ = uint16_t(n);
			buffered_at_ = 0;
		}
		const uint32_t take = std::min({ uint32_t(buffered_), p_frames - written, header_.total_frames - position_ });
		const int16_t *src = &buffer_[size_t(buffered_at_) * lanes];
		float *dst = r_out + size_t(written) * lanes;
		for (size_t i = 0; i < size_t(take) * lanes; ++i) {
			dst[i] = float(src[i]) * (1.0f / 32768.0f);
		}
		buffered_ = uint16_t(buffered_ - take);
		buffered_at_ = uint16_t(buffered_at_ + take);
		written += take;
		position_ += take;
	}
	return written;
}

Status StreamDecoder::skip_to(uint32_t p_frame) {
	if (p_frame < position_) {
		return make_error("sound stream: skip_to(" + std::to_string(p_frame) + ") is behind position " + std::to_string(position_));
	}
	p_frame = std::min(p_frame, header_.total_frames);
	const size_t group_bytes = bytes_per_frame_group(header_);
	const uint32_t group_frames = frames_per_group(header_);
	while (position_ < p_frame) {
		if (buffered_ > 0) {
			const uint32_t take = std::min(uint32_t(buffered_), p_frame - position_);
			buffered_ = uint16_t(buffered_ - take);
			buffered_at_ = uint16_t(buffered_at_ + take);
			position_ += take;
			continue;
		}
		// XAS frames carry their own predictor history, so whole frames before the target are skipped undecoded.
		if (block_frames_left_ > 0 && header_.codec == SoundCodec::EaXas && block_frames_left_ >= group_frames && p_frame - position_ >= group_frames) {
			cursor_ += group_bytes;
			block_frames_left_ -= group_frames;
			position_ += group_frames;
			continue;
		}
		if (block_frames_left_ > 0 && header_.codec == SoundCodec::Pcm16Be) {
			const uint32_t take = std::min(block_frames_left_, p_frame - position_);
			cursor_ += size_t(take) * group_bytes;
			block_frames_left_ -= take;
			position_ += take;
			continue;
		}
		// Decode the frame the target lands in (or open the next block) by reading into a scratch buffer.
		float scratch[kXasFrameSamples * kMaxStreamChannels];
		const uint32_t want = std::min(p_frame - position_, uint32_t(1));
		Result<uint32_t> got = read(scratch, want);
		if (!got) {
			return got.error();
		}
		if (*got == 0) {
			break;
		}
	}
	return Status::success();
}

} // namespace bf3::core::audio
