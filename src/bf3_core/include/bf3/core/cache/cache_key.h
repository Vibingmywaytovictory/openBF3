#pragma once

#include "bf3/core/cache/digest.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {

inline constexpr uint32_t kCacheContainerVersion = 1;
inline constexpr size_t kCacheKindMaxLength = 32;
inline constexpr size_t kCacheKeyFieldMaxLength = 4096;
inline constexpr size_t kCacheKeyMaxSources = 1u << 20;

struct CacheKeyInputs {
	std::string kind;
	std::vector<Sha1Digest> sources;
	uint32_t converter_version = 0;
	std::string engine;
	uint32_t format_version = 0;
	std::string jolt_version;
	std::string params;
};

struct CacheKey {
	std::string kind;
	Sha256Digest digest;

	bool operator==(const CacheKey &) const = default;
	std::string hex() const { return digest.hex(); }

	std::filesystem::path relative_path() const;
};

bool is_valid_cache_kind(std::string_view p_kind);

std::optional<CacheKey> make_cache_key(const CacheKeyInputs &p_inputs);

}
