#pragma once
// On-disk format of one cache entry: a 128-byte header followed by the payload. The payload starts
// at a 64-byte aligned offset of the (page-aligned) read-only mapping.
//
//   off size
//     0    8  magic "OBF3CACH"
//     8    4  container version (kCacheContainerVersion), little endian
//    12    4  header size (128)
//    16   32  key (SHA-256)
//    48    8  payload size
//    56   32  SHA-256 of the payload
//    88   32  kind, zero padded
//   120    4  flags (0)
//   124    4  CRC-32 of bytes 0..123
//   128    -  payload

#include "bf3/core/cache/digest.h"

#include <cstdint>
#include <span>
#include <string>

namespace bf3::core {

inline constexpr size_t kCacheEntryHeaderSize = 128;
inline constexpr uint64_t kCacheMaxPayload = uint64_t(64) << 30; // 64 GiB: far above any blob

struct CacheEntryHeader {
	Sha256Digest key;
	std::string kind;
	uint64_t payload_size = 0;
	Sha256Digest payload_sha256;
};

enum class EntryHeaderCheck {
	Ok,
	TooSmall, // fewer than kCacheEntryHeaderSize bytes
	BadMagic,
	BadVersion,
	BadCrc,
	BadKind,
	KeyMismatch,
	SizeMismatch, // file size != header + payload size (truncated or appended)
};

void encode_entry_header(const CacheEntryHeader &p_header, std::span<uint8_t, kCacheEntryHeaderSize> r_out);

// Parses and checks the header of a whole entry file (p_file = header + payload). Every length is
// checked against p_file.size(); the payload hash is not computed here.
EntryHeaderCheck decode_entry_header(std::span<const uint8_t> p_file, CacheEntryHeader &r_header);
// Same checks from the first kCacheEntryHeaderSize bytes and the file size (open-time scan).
EntryHeaderCheck decode_entry_head(std::span<const uint8_t> p_head, uint64_t p_file_size, CacheEntryHeader &r_header);

const char *entry_header_check_name(EntryHeaderCheck p_check);

} // namespace bf3::core
