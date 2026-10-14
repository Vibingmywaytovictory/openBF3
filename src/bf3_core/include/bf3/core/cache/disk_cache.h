#pragma once
// The derived-data cache (plan §2.1/§2.4, gap load §6.4). Godot-free: the caller passes an absolute
// root (bf3_godot passes the globalised user://bf3cache) and the install's write guard.
//
// Layout under the root:
//   v1/lock                                held exclusively while a Cache is open (one process per root)
//   v1/index.log                           append-only journal: put / touch / remove / pin / close
//   v1/<kind>/<k[0:2]>/<k>.bin             one entry: 128-byte header + payload (entry_format.h)
//   v1/<kind>/<k[0:2]>/<k>.<pid>-<n>.tmp   a write in progress (removed at open after a crash)
//
// Guarantees:
// * Writes are atomic: temp file in the target directory, flush, rename-with-replace on the handle.
//   Readers see the old state or the complete entry, never a partial one.
// * Writes cannot be redirected into a protected root (the install): the cache root is opened as a
//   handle and its final path is checked against the guard; every directory below is opened
//   relative to its parent handle without following reparse points (a junction or symlink inside
//   the tree is refused), the target directory's final path is checked again, and the temp file is
//   created and renamed relative to that verified handle. Swapping a path component for a junction
//   after the check therefore cannot move the write. Hard-linked or reparse-point targets are
//   refused, and deletes go through the same handle checks.
// * Entries are validated when read (magic, version, CRC, key, kind, size; the payload SHA-256 on
//   first use per process or on demand). A damaged or partial entry is deleted and reported as a
//   miss.
// * open() recovers from a crash: stray temp files are removed, a torn journal tail is discarded,
//   the journal is reconciled with the files on disk (entries without a record are adopted as the
//   least recently used, records without a file are dropped) and rewritten when needed.
// * Thread safety: all methods may be called from any thread. Puts of the same key are
//   deduplicated (one writer, the others wait and reuse its entry); different keys proceed in
//   parallel (file I/O runs outside the index lock).
// * Eviction is explicit (gap load §6.4: at round end, never mid-round): least recently used first,
//   skipping persistent pins and entries with a live CacheEntry view.

#include "bf3/core/cache/cache_key.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/path_guard.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::core {

enum class CacheStatus {
	Ok,
	Miss, // not in the cache (or it was damaged and has been dropped; see GetResult)
	Refused, // the guard refused the root or a target (protected root, junction, hard link)
	Busy, // the root is open in another Cache (this or another process)
	Io, // a file system call failed (win32_error says which)
	InvalidArgument,
	Closed, // the Cache is not open
	ProducerFailed, // get_or_put's producer returned false
};

const char *cache_status_name(CacheStatus p_status);

enum class CacheVerify {
	FirstUse, // hash the payload the first time an entry is read in this process (default)
	Always, // hash on every read
	HeaderOnly, // header checks only; hash only on verify() or get(…, true)
};

struct CacheOptions {
	std::filesystem::path root; // absolute; created if missing
	PathGuard guard; // must protect at least the install (InstallReader::write_guard())
	uint64_t budget_bytes = 0; // 0 = unlimited
	uint32_t low_watermark_permille = 900; // enforce_budget() evicts down to budget * this / 1000
	CacheVerify verify = CacheVerify::FirstUse;
	// Testing only: called after the path-level guard check and before the target directory is
	// opened, to let tests swap components of the path in between.
	std::function<void(const std::filesystem::path &p_target)> test_hook_before_write;
};

struct CacheStats {
	uint64_t entries = 0;
	uint64_t bytes = 0; // file bytes of all entries (headers included)
	uint64_t pinned = 0;
	uint64_t hits = 0;
	uint64_t misses = 0;
	uint64_t puts = 0; // entries written
	uint64_t dedup_reused = 0; // puts that found the entry already present
	uint64_t dedup_waits = 0; // puts or producers that waited for another writer of the same key
	uint64_t corrupt_dropped = 0; // damaged entries deleted (open scan or read)
	uint64_t verified = 0; // payload hashes computed on read
	uint64_t evicted = 0;
	uint64_t evicted_bytes = 0;
	uint64_t refused = 0; // writes or deletes the guard refused
	uint64_t io_errors = 0;
	// From the last open():
	uint64_t stray_temps_removed = 0;
	uint64_t journal_torn_bytes = 0; // discarded tail
	uint64_t adopted = 0; // entry files found without a journal record
	uint64_t missing_dropped = 0; // journal records without a file
	uint64_t foreign = 0; // unexpected names in the tree (left alone)
	uint64_t journal_records = 0; // records in the journal file now
	bool journal_compacted = false;
	bool recovered_unclean = false; // the journal did not end with a clean-close record
};

struct PutResult {
	CacheStatus status = CacheStatus::Closed;
	bool written = false; // this call wrote the entry
	bool reused = false; // the entry existed (or another thread wrote it meanwhile)
	uint32_t win32_error = 0;
};

class Cache;

// A read-only memory-mapped entry. While it lives, the entry is pinned against eviction.
class CacheEntry {
public:
	CacheEntry() = default;
	~CacheEntry();
	CacheEntry(CacheEntry &&p_other) noexcept;
	CacheEntry &operator=(CacheEntry &&p_other) noexcept;
	CacheEntry(const CacheEntry &) = delete;
	CacheEntry &operator=(const CacheEntry &) = delete;

	bool valid() const { return view_ != nullptr; }
	std::span<const uint8_t> payload() const { return payload_; }
	const Sha256Digest &key() const { return key_; }
	void reset();

private:
	friend class Cache;
	struct Release;
	std::shared_ptr<Release> owner_; // unpins the slot when the last view goes
	const void *view_ = nullptr;
	std::span<const uint8_t> payload_;
	Sha256Digest key_;
};

struct GetResult {
	CacheStatus status = CacheStatus::Closed;
	CacheEntry entry; // valid() when status == Ok
	bool dropped_corrupt = false; // an entry existed but was damaged; it was deleted (status Miss)
	bool produced = false; // get_or_put: this call ran the producer
	uint32_t win32_error = 0;
};

struct EvictResult {
	CacheStatus status = CacheStatus::Closed;
	std::vector<Sha256Digest> evicted; // in eviction order (least recently used first)
	uint64_t freed_bytes = 0;
	uint64_t skipped_pinned = 0;
	uint64_t skipped_in_use = 0;
	uint64_t delete_failures = 0; // removed from the index; the file is re-adopted at the next open
};

class Cache {
public:
	Cache() = default;
	~Cache();
	Cache(Cache &&) noexcept;
	Cache &operator=(Cache &&) noexcept;
	Cache(const Cache &) = delete;
	Cache &operator=(const Cache &) = delete;

	// Opens (and creates) the cache under p_options.root, recovers from a previous crash.
	CacheStatus open(CacheOptions p_options, std::string *r_detail = nullptr);
	bool is_open() const;
	// Writes pending LRU updates and a clean-close record. Live CacheEntry views stay readable.
	void close();

	// Stores p_payload under p_key unless the key is present already (then reused = true).
	PutResult put(const CacheKey &p_key, std::span<const uint8_t> p_payload);
	// Maps the entry. p_verify forces a payload hash for this read.
	GetResult get(const CacheKey &p_key, bool p_verify = false);
	// get(), or run p_produce once across all threads asking for this key, put, then get.
	GetResult get_or_put(const CacheKey &p_key, const std::function<bool(std::vector<uint8_t> &r_payload)> &p_produce);
	bool contains(const CacheKey &p_key) const;
	// Hashes the payload now. Ok, Miss (absent or dropped as damaged), or an error.
	CacheStatus verify(const CacheKey &p_key);
	CacheStatus remove(const CacheKey &p_key);

	// Persistent pin (journaled): pinned entries are never evicted.
	CacheStatus set_pinned(const CacheKey &p_key, bool p_pinned);

	// Evicts least recently used, unpinned, unused entries until bytes <= p_target_bytes.
	EvictResult evict_to(uint64_t p_target_bytes);
	// When bytes > budget: evict down to budget * low_watermark. No-op without a budget.
	EvictResult enforce_budget();

	// Persists LRU updates (touch records) and flushes the journal to disk.
	CacheStatus flush();
	// Rewrites the journal with one record per live entry (atomic replace).
	CacheStatus compact_journal();

	CacheStats stats() const;
	std::filesystem::path root() const; // the root's final path (after links)

	struct State;

private:
	std::shared_ptr<State> state_;
};

} // namespace bf3::core
