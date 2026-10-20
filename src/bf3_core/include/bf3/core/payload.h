#pragma once

#include "bf3/core/bundle_manifest.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::core {

constexpr uint32_t kMaxBlockLen = 0x10000;
constexpr size_t kBlockHeaderLen = 8;

struct Block {
	uint64_t stored_at = 0;
	uint32_t stored_len = 0;
	uint64_t decoded_at = 0;
	uint32_t decoded_len = 0;
	bool is_verbatim() const { return stored_len == kMaxBlockLen && decoded_len == kMaxBlockLen; }
};

inline bool chunk_is_framed(const Guid &p_id) {
	return (p_id.bytes[15] & 1) != 0;
}

Result<std::vector<Block>> payload_blocks(std::span<const uint8_t> p_stored, uint64_t p_base = 0);
Result<uint64_t> decoded_len(std::span<const uint8_t> p_stored);
Result<std::vector<uint8_t>> decode_blocks(std::span<const uint8_t> p_stored, uint64_t p_base = 0);

Status decode_blocks_into(std::span<const uint8_t> p_stored, std::vector<uint8_t> &r_out, uint64_t p_base = 0);
Result<std::vector<uint8_t>> decode_chunk(const Guid &p_id, std::span<const uint8_t> p_stored);

Result<std::vector<uint8_t>> decode_chunk_range(const Guid &p_id, std::span<const uint8_t> p_stored, const ChunkRange &p_range);

}
