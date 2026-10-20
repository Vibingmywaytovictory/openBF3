#include "bf3/core/cache/cached_convert.h"

#ifndef OPENBF3_ENGINE_ID
#error "OPENBF3_ENGINE_ID must name the pinned engine API (set by src/bf3_core/CMakeLists.txt)"
#endif

namespace bf3::core {

std::string_view cache_engine_id() {
	return OPENBF3_ENGINE_ID;
}

const char *cache_outcome_name(CacheOutcome p_outcome) {
	switch (p_outcome) {
		case CacheOutcome::Hit:
			return "hit";
		case CacheOutcome::Converted:
			return "converted";
		case CacheOutcome::Reused:
			return "reused";
		case CacheOutcome::Uncached:
		default:
			return "uncached";
	}
}

Result<CachedItem> cached_convert(Cache *p_cache, const CacheKey &p_key, const std::function<Status(std::span<const uint8_t>)> &p_accept,
		const std::function<Result<std::vector<uint8_t>>()> &p_produce) {
	CachedItem item;
	const bool use_cache = p_cache != nullptr && p_cache->is_open();
	if (use_cache) {
		GetResult got = p_cache->get(p_key);
		if (got.status == CacheStatus::Ok) {
			const Status accepted = p_accept(got.entry.payload());
			if (accepted.ok()) {
				item.outcome = CacheOutcome::Hit;
				item.payload_bytes = got.entry.payload().size();
				return item;
			}

			got.entry.reset();
			p_cache->remove(p_key);
			item.dropped_corrupt = true;
		} else if (got.status == CacheStatus::Miss) {
			item.dropped_corrupt = got.dropped_corrupt;
		} else {
			item.cache_error = std::string("get: ") + cache_status_name(got.status);
		}
	}
	Result<std::vector<uint8_t>> blob = p_produce();
	if (!blob) {
		return blob.error();
	}
	item.payload_bytes = blob->size();
	item.outcome = CacheOutcome::Uncached;
	if (use_cache) {
		const PutResult put = p_cache->put(p_key, *blob);
		if (put.status == CacheStatus::Ok) {
			item.outcome = put.written ? CacheOutcome::Converted : CacheOutcome::Reused;
		} else {
			item.cache_error = std::string("put: ") + cache_status_name(put.status);
			if (put.win32_error != 0) {
				item.cache_error += " (error " + std::to_string(put.win32_error) + ")";
			}
		}
	}
	return item;
}

void ConvertTotals::add(const CachedItem &p_item) {
	++items;
	if (p_item.dropped_corrupt) {
		++dropped_corrupt;
	}
	switch (p_item.outcome) {
		case CacheOutcome::Hit:
			++hits;
			bytes_hit += p_item.payload_bytes;
			break;
		case CacheOutcome::Converted:
			++converted;
			bytes_written += p_item.payload_bytes;
			break;
		case CacheOutcome::Reused:
			++reused;
			break;
		case CacheOutcome::Uncached:
			++uncached;
			break;
	}
}

}
