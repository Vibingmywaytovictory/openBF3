#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/result.h"
#include "bf3/fx/effect_def.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::fx {

inline constexpr uint32_t kFxBlobFormatVersion = 1;
inline constexpr uint32_t kFxDepsFormatVersion = 1;

inline constexpr uint32_t kFxConverterVersion = 1;
inline constexpr const char *kFxEffectCacheKind = "fx-effect";
inline constexpr const char *kFxDepsCacheKind = "fx-deps";

std::vector<uint8_t> encode_effect_def(const EffectDef &p_def);
core::Result<EffectDef> decode_effect_def(std::span<const uint8_t> p_blob);

std::vector<uint8_t> encode_effect_deps(const std::vector<std::string> &p_sources);
core::Result<std::vector<std::string>> decode_effect_deps(std::span<const uint8_t> p_blob);

using PartitionDigest = core::gameplay::PartitionDigest;

struct FxSources {
	const core::EbxResolver *db = nullptr;
	PartitionDigest digest;
};

std::optional<core::CacheKey> effect_deps_key(const FxSources &p_sources, std::string_view p_blueprint);
std::optional<core::CacheKey> effect_def_key(const FxSources &p_sources, std::string_view p_blueprint, const std::vector<std::string> &p_deps);

struct CachedEffect {
	EffectDef def;
	core::CachedItem item;
	size_t dependencies = 0;
};

core::Result<CachedEffect> compile_effect_cached(core::Cache *p_cache, const FxSources &p_sources, std::string_view p_blueprint,
		core::ConvertTotals *r_totals = nullptr);

}
