#pragma once

#include "bf3/core/cache/cache_key.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {

std::string_view cache_engine_id();

inline Sha1Digest source_digest(const Sha1 &p_sha1) {
	Sha1Digest d;
	d.bytes = p_sha1.bytes;
	return d;
}

enum class CacheOutcome : uint8_t {
	Hit,
	Converted,
	Reused,
	Uncached,
};
const char *cache_outcome_name(CacheOutcome p_outcome);

struct CachedItem {
	CacheOutcome outcome = CacheOutcome::Uncached;
	bool dropped_corrupt = false;
	uint64_t payload_bytes = 0;
	std::string cache_error;
};

Result<CachedItem> cached_convert(Cache *p_cache, const CacheKey &p_key, const std::function<Status(std::span<const uint8_t>)> &p_accept,
		const std::function<Result<std::vector<uint8_t>>()> &p_produce);

struct ConvertTotals {
	std::atomic<uint64_t> items{ 0 };
	std::atomic<uint64_t> hits{ 0 };
	std::atomic<uint64_t> converted{ 0 };
	std::atomic<uint64_t> reused{ 0 };
	std::atomic<uint64_t> uncached{ 0 };
	std::atomic<uint64_t> failed{ 0 };
	std::atomic<uint64_t> dropped_corrupt{ 0 };
	std::atomic<uint64_t> bytes_hit{ 0 };
	std::atomic<uint64_t> bytes_written{ 0 };

	void add(const CachedItem &p_item);
	void add_failure() {
		++items;
		++failed;
	}
};

}
