#pragma once

#include "bf3/core/cache/digest.h"

#include <cstdint>
#include <span>
#include <string>

namespace bf3::core {

inline constexpr size_t kCacheEntryHeaderSize = 128;
inline constexpr uint64_t kCacheMaxPayload = uint64_t(64) << 30;

struct CacheEntryHeader {
	Sha256Digest key;
	std::string kind;
	uint64_t payload_size = 0;
	Sha256Digest payload_sha256;
};

enum class EntryHeaderCheck {
	Ok,
	TooSmall,
	BadMagic,
	BadVersion,
	BadCrc,
	BadKind,
	KeyMismatch,
	SizeMismatch,
};

void encode_entry_header(const CacheEntryHeader &p_header, std::span<uint8_t, kCacheEntryHeaderSize> r_out);

EntryHeaderCheck decode_entry_header(std::span<const uint8_t> p_file, CacheEntryHeader &r_header);

EntryHeaderCheck decode_entry_head(std::span<const uint8_t> p_head, uint64_t p_file_size, CacheEntryHeader &r_header);

const char *entry_header_check_name(EntryHeaderCheck p_check);

}
