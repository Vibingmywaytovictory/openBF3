#pragma once
// One converted item through the derived-data cache (plan §2.4 step 3, §3.12): look the key up, accept the
// cached blob if it decodes, otherwise convert, put and use the conversion. Godot-free; every converter
// (meshes in bf3_render, WeaponDefs in gameplay/) runs its items through cached_convert, one item at a time,
// so a converter's peak memory is that of the items in flight (plan §2.3 loader-thread rule).
//
//   hit        the entry exists, passed the cache's checks (header, CRC, payload SHA-256 on first use) and
//              p_accept decoded it
//   converted  no usable entry: p_produce ran and this call wrote the entry
//   reused     p_produce ran, but another thread (or an identical-content item) had written the key meanwhile
//   uncached   no cache (null or closed), or the put failed: p_produce ran and nothing was stored
// A cached blob that fails p_accept (it passed the SHA-256 but the decoder refuses it: a hostile or
// out-of-date writer) is removed and reconverted, like an entry the cache itself found damaged
// (dropped_corrupt in both cases).
//
// Engine id: keys carry the pinned engine API version+commit (the engine-api/<version>.<hash> folder the
// build uses, "4.8.dev7.c971f93e7"), so the game and bf3dump compute the same keys and an engine bump
// re-converts.

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

// The pinned engine API (OPENBF3_ENGINE_API's folder name), e.g. "4.8.dev7.c971f93e7".
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
	bool dropped_corrupt = false; // a damaged or undecodable entry was removed before converting
	uint64_t payload_bytes = 0; // bytes of the blob read (hit) or produced
	std::string cache_error; // why a put or lookup failed (uncached despite an open cache)
};

// Gets p_key from p_cache (may be null); p_accept decodes a cached payload (an error = undecodable). On a miss
// p_produce converts and returns the blob (the caller keeps its own decoded result), which is put. Fails only
// when p_produce fails.
Result<CachedItem> cached_convert(Cache *p_cache, const CacheKey &p_key, const std::function<Status(std::span<const uint8_t>)> &p_accept,
		const std::function<Result<std::vector<uint8_t>>()> &p_produce);

// Totals over many items; safe to add to from several threads.
struct ConvertTotals {
	std::atomic<uint64_t> items{ 0 };
	std::atomic<uint64_t> hits{ 0 };
	std::atomic<uint64_t> converted{ 0 };
	std::atomic<uint64_t> reused{ 0 };
	std::atomic<uint64_t> uncached{ 0 };
	std::atomic<uint64_t> failed{ 0 };
	std::atomic<uint64_t> dropped_corrupt{ 0 };
	std::atomic<uint64_t> bytes_hit{ 0 }; // payload bytes read from the cache
	std::atomic<uint64_t> bytes_written{ 0 }; // payload bytes this run wrote

	void add(const CachedItem &p_item);
	void add_failure() {
		++items;
		++failed;
	}
};

} // namespace bf3::core
