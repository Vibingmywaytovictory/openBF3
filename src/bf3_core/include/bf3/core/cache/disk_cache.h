#pragma once

#include "bf3/core/cache/cache_key.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/path_guard.h"

#include <atomic>
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
	Miss,
	Refused,
	Busy,
	Io,
	InvalidArgument,
	Closed,
	ProducerFailed,
};

const char *cache_status_name(CacheStatus p_status);

enum class CacheVerify {
	FirstUse,
	Always,
	HeaderOnly,
};

struct CacheOptions {
	std::filesystem::path root;
	PathGuard guard;
	uint64_t budget_bytes = 0;
	uint32_t low_watermark_permille = 900;
	CacheVerify verify = CacheVerify::FirstUse;

	std::function<void(const std::filesystem::path &p_target)> test_hook_before_write;
};

struct CacheStats {
	uint64_t entries = 0;
	uint64_t bytes = 0;
	uint64_t pinned = 0;
	uint64_t hits = 0;
	uint64_t misses = 0;
	uint64_t puts = 0;
	uint64_t dedup_reused = 0;
	uint64_t dedup_waits = 0;
	uint64_t corrupt_dropped = 0;
	uint64_t verified = 0;
	uint64_t evicted = 0;
	uint64_t evicted_bytes = 0;
	uint64_t refused = 0;
	uint64_t io_errors = 0;

	uint64_t stray_temps_removed = 0;
	uint64_t journal_torn_bytes = 0;
	uint64_t adopted = 0;
	uint64_t missing_dropped = 0;
	uint64_t foreign = 0;
	uint64_t journal_records = 0;
	bool journal_compacted = false;
	bool recovered_unclean = false;
};

struct PutResult {
	CacheStatus status = CacheStatus::Closed;
	bool written = false;
	bool reused = false;
	uint32_t win32_error = 0;
};

class Cache;

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
	std::shared_ptr<Release> owner_;
	const void *view_ = nullptr;
	std::span<const uint8_t> payload_;
	Sha256Digest key_;
};

struct GetResult {
	CacheStatus status = CacheStatus::Closed;
	CacheEntry entry;
	bool dropped_corrupt = false;
	bool produced = false;
	uint32_t win32_error = 0;
};

struct EvictResult {
	CacheStatus status = CacheStatus::Closed;
	std::vector<Sha256Digest> evicted;
	uint64_t freed_bytes = 0;
	uint64_t skipped_pinned = 0;
	uint64_t skipped_in_use = 0;
	uint64_t delete_failures = 0;
};

class Cache {
public:
	Cache() = default;
	~Cache();
	Cache(Cache &&) noexcept;
	Cache &operator=(Cache &&) noexcept;
	Cache(const Cache &) = delete;
	Cache &operator=(const Cache &) = delete;

	CacheStatus open(CacheOptions p_options, std::string *r_detail = nullptr);
	bool is_open() const;

	void close();

	PutResult put(const CacheKey &p_key, std::span<const uint8_t> p_payload);

	GetResult get(const CacheKey &p_key, bool p_verify = false);

	GetResult get_or_put(const CacheKey &p_key, const std::function<bool(std::vector<uint8_t> &r_payload)> &p_produce);
	bool contains(const CacheKey &p_key) const;

	CacheStatus verify(const CacheKey &p_key);
	CacheStatus remove(const CacheKey &p_key);

	CacheStatus set_pinned(const CacheKey &p_key, bool p_pinned);

	EvictResult evict_to(uint64_t p_target_bytes);

	EvictResult enforce_budget();

	CacheStatus flush();

	CacheStatus compact_journal();

	CacheStats stats() const;
	std::filesystem::path root() const;

	struct State;

private:
	static GetResult get_in(const std::shared_ptr<State> &p_state, const CacheKey &p_key, bool p_verify);

	std::atomic<std::shared_ptr<State>> state_;
};

}
