#include "bf3/core/audio/sound_stream.h"

#include <algorithm>
#include <cstring>
#include <string>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define OPENBF3_XAS_SSE2 1
#else
#define OPENBF3_XAS_SSE2 0
#endif

namespace bf3::core::audio {

namespace {

constexpr int32_t kXasCoefficients[4][2] = { { 0, 0 }, { 240, 0 }, { 460, -208 }, { 392, -220 } };
constexpr size_t kBlockHeaderBytes = 4;
constexpr size_t kDataHeaderBytes = 8;

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

	return p_header.codec == SoundCodec::EaXas ? kXasFrameBytes * p_header.channels : size_t(2) * p_header.channels;
}
uint32_t frames_per_group(const SoundStreamHeader &p_header) {
	return p_header.codec == SoundCodec::EaXas ? kXasFrameSamples : 1u;
}

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

Result<uint32_t> data_block_frames(std::span<const uint8_t> p_chunk, size_t p_at, const Block &p_block, const SoundStreamHeader &p_header) {
	const uint32_t frames = be32(&p_chunk[p_at + kBlockHeaderBytes]);
	const size_t body = p_block.size - kDataHeaderBytes;
	const uint64_t capacity = uint64_t(body / bytes_per_frame_group(p_header)) * frames_per_group(p_header);
	if (frames > capacity) {
		return stream_error(p_at, "data block declares " + std::to_string(frames) + " frames, its body holds " + std::to_string(capacity));
	}
	return frames;
}

}

void decode_xas_frame(std::span<const uint8_t, kXasFrameBytes> p_frame, std::array<int16_t, kXasFrameSamples> &r_out) {

#if OPENBF3_XAS_SSE2
	alignas(16) int16_t c16[8], h16[8], m2[8], m1[8];
	for (size_t group = 0; group < 4; ++group) {
		const uint32_t header = uint32_t(p_frame[group * 4]) | (uint32_t(p_frame[group * 4 + 1]) << 8) | (uint32_t(p_frame[group * 4 + 2]) << 16) |
				(uint32_t(p_frame[group * 4 + 3]) << 24);
		const int16_t h2 = int16_t(uint16_t(header & 0xfff0));
		const int16_t h1 = int16_t(uint16_t((header >> 16) & 0xfff0));
		const int32_t shift = 20 - int32_t((header >> 16) & 0x0f);
		const int32_t s1 = std::min(shift, int32_t(14));
		c16[group * 2] = int16_t(kXasCoefficients[header & 3][0]);
		c16[group * 2 + 1] = int16_t(kXasCoefficients[header & 3][1]);
		h16[group * 2] = h1;
		h16[group * 2 + 1] = h2;
		m2[group] = int16_t(1 << (shift - s1));
		m2[group + 4] = 0;
		m1[group * 2] = int16_t(1 << s1);
		m1[group * 2 + 1] = 0;
		const size_t base = group * kXasGroupSamples;
		r_out[base] = h2;
		r_out[base + 1] = h1;
	}
	const __m128i zero = _mm_setzero_si128();
	const __m128i coef = _mm_load_si128(reinterpret_cast<const __m128i *>(c16));
	const __m128i mul2 = _mm_load_si128(reinterpret_cast<const __m128i *>(m2));
	const __m128i mul1 = _mm_load_si128(reinterpret_cast<const __m128i *>(m1));
	const __m128i round = _mm_set1_epi32(128);
	__m128i hist = _mm_load_si128(reinterpret_cast<const __m128i *>(h16));
	alignas(16) int16_t steps[30][4];
	for (size_t row = 0; row < 15; ++row) {
		uint32_t bytes;
		std::memcpy(&bytes, &p_frame[16 + row * 4], 4);
		const __m128i b = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int32_t(bytes)), zero);
		const __m128i hi = _mm_srai_epi16(_mm_slli_epi16(_mm_srli_epi16(b, 4), 12), 12);
		const __m128i lo = _mm_srai_epi16(_mm_slli_epi16(b, 12), 12);
		const __m128i terms[2] = {
			_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(_mm_mullo_epi16(hi, mul2), zero), mul1), round),
			_mm_add_epi32(_mm_madd_epi16(_mm_unpacklo_epi16(_mm_mullo_epi16(lo, mul2), zero), mul1), round),
		};
		for (size_t half = 0; half < 2; ++half) {
			const __m128i sample = _mm_srai_epi32(_mm_add_epi32(terms[half], _mm_madd_epi16(hist, coef)), 8);
			const __m128i packed = _mm_packs_epi32(sample, sample);
			_mm_storel_epi64(reinterpret_cast<__m128i *>(steps[row * 2 + half]), packed);
			hist = _mm_or_si128(_mm_slli_epi32(hist, 16), _mm_unpacklo_epi16(packed, zero));
		}
	}
	for (size_t group = 0; group < 4; ++group) {
		for (size_t step = 0; step < 30; ++step) {
			r_out[group * kXasGroupSamples + 2 + step] = steps[step][group];
		}
	}
#else
	int32_t coef1[4], coef2[4], hist1[4], hist2[4], shift[4];
	for (size_t group = 0; group < 4; ++group) {
		const uint32_t header = uint32_t(p_frame[group * 4]) | (uint32_t(p_frame[group * 4 + 1]) << 8) | (uint32_t(p_frame[group * 4 + 2]) << 16) |
				(uint32_t(p_frame[group * 4 + 3]) << 24);
		coef1[group] = kXasCoefficients[header & 3][0];
		coef2[group] = kXasCoefficients[header & 3][1];
		hist2[group] = int32_t(int16_t(uint16_t(header & 0xfff0)));
		hist1[group] = int32_t(int16_t(uint16_t((header >> 16) & 0xfff0)));
		shift[group] = 20 - int32_t((header >> 16) & 0x0f);
		const size_t base = group * kXasGroupSamples;
		r_out[base] = int16_t(hist2[group]);
		r_out[base + 1] = int16_t(hist1[group]);
	}
	for (size_t row = 0; row < 15; ++row) {
		for (size_t half = 0; half < 2; ++half) {
			for (size_t group = 0; group < 4; ++group) {
				const uint8_t byte = p_frame[16 + row * 4 + group];
				const uint8_t nib = half == 0 ? uint8_t(byte >> 4) : uint8_t(byte & 0x0f);
				const int32_t nibble = int32_t(int8_t(uint8_t(nib << 4))) >> 4;
				int32_t sample = ((nibble * (int32_t(1) << shift[group])) + hist1[group] * coef1[group] + hist2[group] * coef2[group] + 128) >> 8;
				sample = std::clamp(sample, int32_t(-32768), int32_t(32767));
				r_out[group * kXasGroupSamples + 2 + row * 2 + half] = int16_t(sample);
				hist2[group] = hist1[group];
				hist1[group] = sample;
			}
		}
	}
#endif
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

	const size_t after = p_chunk.size() - p_offset - block->size;
	const uint64_t most = uint64_t(after / bytes_per_frame_group(out)) * frames_per_group(out);
	if (out.total_frames > most) {
		return stream_error(p_offset, std::to_string(out.total_frames) + " frames declared, the " + std::to_string(after) + " bytes after the header hold at most " +
				std::to_string(most));
	}
	return out;
}

namespace {

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
			break;
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

}

Result<StreamScan> scan_stream(std::span<const uint8_t> p_chunk, size_t p_offset, bool p_decode) {
	Result<SoundStreamHeader> parsed = parse_stream_header(p_chunk, p_offset);
	if (!parsed) {
		return parsed.error();
	}
	const SoundStreamHeader header = *parsed;
	volatile int32_t sink = 0;
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
			const uint32_t n = std::min(block_frames_left_, kXasFrameSamples);
			if (header_.codec == SoundCodec::EaXas) {
				std::array<int16_t, kXasFrameSamples> one{};
				if (lanes == 1) {
					decode_xas_frame(std::span<const uint8_t, kXasFrameBytes>(chunk_ + cursor_, kXasFrameBytes), one);
					std::memcpy(buffer_.data(), one.data(), size_t(n) * sizeof(int16_t));
				} else if (lanes == 2) {
					std::array<int16_t, kXasFrameSamples> two{};
					decode_xas_frame(std::span<const uint8_t, kXasFrameBytes>(chunk_ + cursor_, kXasFrameBytes), one);
					decode_xas_frame(std::span<const uint8_t, kXasFrameBytes>(chunk_ + cursor_ + kXasFrameBytes, kXasFrameBytes), two);
					for (uint32_t i = 0; i < n; ++i) {
						buffer_[i * 2] = one[i];
						buffer_[i * 2 + 1] = two[i];
					}
				}
				for (size_t ch = 0; lanes > 2 && ch < lanes; ++ch) {
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

}
