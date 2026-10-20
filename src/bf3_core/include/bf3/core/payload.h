#pragma once
// Block framing of res payloads, flagged chunks and framed idata. Ported from IW4L
// superbundle_bf3 (payload.rs); inflate by libdeflate instead of flate2.
//
//   payload := block+      block := u32 BE decodedSize | u32 BE storedSize | data[storedSize]
//   decodedSize <= 0x10000. VERBATIM iff storedSize == decodedSize == 0x10000, else data is a
//   zlib stream inflating to exactly decodedSize. (stored == decoded alone does NOT mean verbatim.)
// A chunk is framed iff byte 15 of its canonical id has bit 0 set.

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
	uint64_t stored_at = 0; // header position in the stored bytes
	uint32_t stored_len = 0;
	uint64_t decoded_at = 0;
	uint32_t decoded_len = 0;
	bool is_verbatim() const { return stored_len == kMaxBlockLen && decoded_len == kMaxBlockLen; }
};

inline bool chunk_is_framed(const Guid &p_id) {
	return (p_id.bytes[15] & 1) != 0;
}

// p_base is added to the byte positions in error messages when p_stored is a range of a larger payload.
Result<std::vector<Block>> payload_blocks(std::span<const uint8_t> p_stored, uint64_t p_base = 0);
Result<uint64_t> decoded_len(std::span<const uint8_t> p_stored);
Result<std::vector<uint8_t>> decode_blocks(std::span<const uint8_t> p_stored, uint64_t p_base = 0);
// Appends the decoded bytes to r_out.
Status decode_blocks_into(std::span<const uint8_t> p_stored, std::vector<uint8_t> &r_out, uint64_t p_base = 0);
Result<std::vector<uint8_t>> decode_chunk(const Guid &p_id, std::span<const uint8_t> p_stored);
// Decodes stored[range.start..range.end]; range.start must be a block boundary whose preceding
// blocks decode to exactly range.logical_offset bytes.
Result<std::vector<uint8_t>> decode_chunk_range(const Guid &p_id, std::span<const uint8_t> p_stored, const ChunkRange &p_range);

} // namespace bf3::core
