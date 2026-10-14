#pragma once
// The cache journal (internal): "v1/index.log", append-only.
//
//   header  : "OBF3JRNL" | u32 version (1) | u32 reserved                       16 bytes
//   record  : u32 body length | u32 CRC-32 of body | body                        8 + n bytes
//   body    : u8 type | fields (little endian)
//     1 Put    key[32] | u64 payload size | u64 tick | u8 kind length | kind
//     2 Touch  key[32] | u64 tick
//     3 Remove key[32] | u8 reason (1 evict, 2 corrupt, 3 explicit)
//     4 Pin    key[32] | u8 pinned
//     5 Close  u64 tick                                     (clean shutdown marker)
//
// Replay stops at the first record that is cut short or fails its CRC (a torn tail after a crash);
// everything before it is kept. Ticks are a logical LRU clock.

#include "bf3/core/cache/digest.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core::cache_detail {

inline constexpr size_t kJournalHeaderSize = 16;
inline constexpr uint32_t kJournalMaxBody = 1024;

enum class RemoveReason : uint8_t {
	Evict = 1,
	Corrupt = 2,
	Explicit = 3,
};

struct JournalEntry {
	std::string kind;
	uint64_t payload_size = 0;
	uint64_t tick = 0;
	bool pinned = false;
};

struct JournalReplay {
	std::unordered_map<Sha256Digest, JournalEntry, DigestHash> entries;
	bool header_ok = false;
	bool clean_close = false; // the last record is Close
	uint64_t max_tick = 0;
	uint64_t records = 0; // valid records
	uint64_t valid_bytes = 0; // header + valid records
	uint64_t torn_bytes = 0; // bytes after the last valid record
};

void journal_header(std::vector<uint8_t> &r_out);
void journal_put(std::vector<uint8_t> &r_out, const Sha256Digest &p_key, std::string_view p_kind, uint64_t p_payload_size, uint64_t p_tick);
void journal_touch(std::vector<uint8_t> &r_out, const Sha256Digest &p_key, uint64_t p_tick);
void journal_remove(std::vector<uint8_t> &r_out, const Sha256Digest &p_key, RemoveReason p_reason);
void journal_pin(std::vector<uint8_t> &r_out, const Sha256Digest &p_key, bool p_pinned);
void journal_close(std::vector<uint8_t> &r_out, uint64_t p_tick);

// Replays a whole journal file. Never reads outside p_bytes.
JournalReplay replay_journal(std::span<const uint8_t> p_bytes);

} // namespace bf3::core::cache_detail
