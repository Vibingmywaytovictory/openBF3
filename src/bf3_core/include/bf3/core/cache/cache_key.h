#pragma once
// Cache keys (plan §2.1, gap load §6.4). A key is the SHA-256 of a canonical, length-prefixed
// encoding of everything that determines a derived blob:
//   kind | source digests (the CAS SHA1s the bundle manifests carry, in converter order) |
//   converter version | engine version+commit | blob format version | [Jolt version] | [params]
// A patched install changes a source SHA1, a new converter or engine changes its field, so stale
// blobs are never found; they age out through LRU eviction. Keys are by content, never by name.

#include "bf3/core/cache/digest.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {

// Version of the on-disk container (entry header, journal, layout). It names the top directory
// "v<n>", so a container change starts an empty tree next to the old one.
inline constexpr uint32_t kCacheContainerVersion = 1;
inline constexpr size_t kCacheKindMaxLength = 32;
inline constexpr size_t kCacheKeyFieldMaxLength = 4096;
inline constexpr size_t kCacheKeyMaxSources = 1u << 20;

struct CacheKeyInputs {
	std::string kind; // [a-z0-9_-]{1,32}; also the directory name ("mesh", "jolt-shape", "nav")
	std::vector<Sha1Digest> sources; // CAS SHA1s of the source payloads, order defined by the converter
	uint32_t converter_version = 0;
	std::string engine; // engine version + commit, e.g. "4.8.dev7.c971f93e7"
	uint32_t format_version = 0; // format of the derived blob
	std::string jolt_version; // empty when the blob does not depend on Jolt
	std::string params; // other output-changing inputs in canonical text (e.g. Recast params); may be empty
};

struct CacheKey {
	std::string kind;
	Sha256Digest digest;

	bool operator==(const CacheKey &) const = default;
	std::string hex() const { return digest.hex(); }
	// "v1/<kind>/<hex[0:2]>/<hex>.bin" (generic '/' separators), relative to the cache root.
	std::filesystem::path relative_path() const;
};

bool is_valid_cache_kind(std::string_view p_kind);

// nullopt when the kind is invalid or a field exceeds its limit.
std::optional<CacheKey> make_cache_key(const CacheKeyInputs &p_inputs);

} // namespace bf3::core
