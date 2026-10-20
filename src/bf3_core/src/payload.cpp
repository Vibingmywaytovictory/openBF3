// Ported from IW4L superbundle_bf3 (payload.rs), Apache-2.0. Inflate: libdeflate (MIT).
#include "bf3/core/payload.h"

#include "format_errors.h"

#include <libdeflate.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace bf3::core {

namespace {

enum class BlockProblem {
	HeaderCut,
	DataCut,
	TooLarge,
	Inflate,
	InflatedLength,
	RangeNotOnBlock,
	RangeLogicalOffset,
};

Error block_error(uint64_t p_at, BlockProblem p_problem) {
	const char *problem = "";
	switch (p_problem) {
		case BlockProblem::HeaderCut:
			problem = "block header runs past the payload";
			break;
		case BlockProblem::DataCut:
			problem = "block data runs past the payload";
			break;
		case BlockProblem::TooLarge:
			problem = "block claims more than 64 KiB";
			break;
		case BlockProblem::Inflate:
			problem = "zlib stream is invalid";
			break;
		case BlockProblem::InflatedLength:
			problem = "zlib stream inflates to the wrong length";
			break;
		case BlockProblem::RangeNotOnBlock:
			problem = "range start is not a block boundary";
			break;
		case BlockProblem::RangeLogicalOffset:
			problem = "blocks before the range do not add up to its logical offset";
			break;
	}
	return make_error("payload block at byte " + std::to_string(p_at) + ": " + problem);
}

uint32_t be32(const uint8_t *p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

struct DecompressorDeleter {
	void operator()(libdeflate_decompressor *p) const { libdeflate_free_decompressor(p); }
};

// One decompressor per thread (libdeflate decompressors are not thread-safe, and cheap to keep).
libdeflate_decompressor *thread_decompressor() {
	thread_local std::unique_ptr<libdeflate_decompressor, DecompressorDeleter> d(libdeflate_alloc_decompressor());
	return d.get();
}

} // namespace

Result<std::vector<Block>> payload_blocks(std::span<const uint8_t> p_stored, uint64_t p_base) {
	std::vector<Block> out;
	size_t at = 0;
	uint64_t decoded_at = 0;
	while (at < p_stored.size()) {
		if (p_stored.size() - at < kBlockHeaderLen) {
			return block_error(p_base + at, BlockProblem::HeaderCut);
		}
		const uint32_t decoded = be32(p_stored.data() + at);
		const uint32_t stored = be32(p_stored.data() + at + 4);
		if (decoded > kMaxBlockLen) {
			return block_error(p_base + at, BlockProblem::TooLarge);
		}
		const size_t data_end = at + kBlockHeaderLen + stored;
		if (data_end > p_stored.size()) {
			return block_error(p_base + at, BlockProblem::DataCut);
		}
		out.push_back(Block{ at, stored, decoded_at, decoded });
		decoded_at += decoded;
		at = data_end;
	}
	return out;
}

Result<uint64_t> decoded_len(std::span<const uint8_t> p_stored) {
	Result<std::vector<Block>> blocks = payload_blocks(p_stored);
	if (!blocks) {
		return blocks.error();
	}
	return blocks->empty() ? 0 : blocks->back().decoded_at + blocks->back().decoded_len;
}

Status decode_blocks_into(std::span<const uint8_t> p_stored, std::vector<uint8_t> &r_out, uint64_t p_base) {
	Result<std::vector<Block>> layout = payload_blocks(p_stored, p_base);
	if (!layout) {
		return layout.error();
	}
	const uint64_t total = layout->empty() ? 0 : layout->back().decoded_at + layout->back().decoded_len;
	const size_t base = r_out.size();
	// No exceptions: resize() on failure would terminate, so check the size first (a block decodes
	// to at most 64 KiB, so a corrupt layout could otherwise ask for 8192x the stored size).
	if (total > detail::kMaxMetadataSizedAlloc || total > r_out.max_size() - base) {
		return detail::out_of_memory(total);
	}
	r_out.resize(base + static_cast<size_t>(total));
	libdeflate_decompressor *inflater = thread_decompressor();
	if (inflater == nullptr) {
		return detail::out_of_memory(sizeof(void *));
	}
	for (const Block &block : *layout) {
		const uint8_t *data = p_stored.data() + block.stored_at + kBlockHeaderLen;
		uint8_t *dst = r_out.data() + base + block.decoded_at;
		if (block.is_verbatim()) {
			std::memcpy(dst, data, block.stored_len);
			continue;
		}
		size_t in_used = 0, out_len = 0;
		const libdeflate_result r =
				libdeflate_zlib_decompress_ex(inflater, data, block.stored_len, dst, block.decoded_len, &in_used, &out_len);
		if (r == LIBDEFLATE_SUCCESS) {
			if (out_len != block.decoded_len) {
				return block_error(p_base + block.stored_at, BlockProblem::InflatedLength);
			}
			continue;
		}
		// Not a complete zlib stream that fits (flate2 would not reach StreamEnd): the IW4L
		// reader then takes the block as stored when the sizes are equal (never needed on retail).
		if (block.stored_len == block.decoded_len) {
			std::memcpy(dst, data, block.stored_len);
			continue;
		}
		return block_error(p_base + block.stored_at, r == LIBDEFLATE_INSUFFICIENT_SPACE ? BlockProblem::InflatedLength : BlockProblem::Inflate);
	}
	return {};
}

Result<std::vector<uint8_t>> decode_blocks(std::span<const uint8_t> p_stored, uint64_t p_base) {
	std::vector<uint8_t> out;
	BF3_TRY(decode_blocks_into(p_stored, out, p_base));
	return out;
}

Result<std::vector<uint8_t>> decode_chunk(const Guid &p_id, std::span<const uint8_t> p_stored) {
	if (chunk_is_framed(p_id)) {
		return decode_blocks(p_stored);
	}
	return std::vector<uint8_t>(p_stored.begin(), p_stored.end());
}

Result<std::vector<uint8_t>> decode_chunk_range(const Guid &p_id, std::span<const uint8_t> p_stored, const ChunkRange &p_range) {
	const size_t start = p_range.start, end = p_range.end;
	if (start > end || end > p_stored.size()) {
		return detail::truncated("ranged chunk", end, p_stored.size());
	}
	if (!chunk_is_framed(p_id)) {
		return std::vector<uint8_t>(p_stored.begin() + start, p_stored.begin() + end);
	}
	size_t at = 0;
	uint64_t decoded = 0;
	while (at < start) {
		if (p_stored.size() - at < kBlockHeaderLen) {
			return block_error(at, BlockProblem::HeaderCut);
		}
		decoded += be32(p_stored.data() + at);
		at += kBlockHeaderLen + be32(p_stored.data() + at + 4);
	}
	if (at != start) {
		return block_error(start, BlockProblem::RangeNotOnBlock);
	}
	if (decoded != p_range.logical_offset) {
		return block_error(start, BlockProblem::RangeLogicalOffset);
	}
	// Block offsets are reported in the whole payload: the range's start is the decoder's base.
	return decode_blocks(p_stored.subspan(start, end - start), start);
}

} // namespace bf3::core
