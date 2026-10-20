#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class LevelMount;
}

namespace bf3::core::gameplay {

inline constexpr uint32_t kWeaponBlobFormatVersion = 2;

inline constexpr uint32_t kWeaponConverterVersion = 2;
inline constexpr const char *kWeaponCacheKind = "weapon";
inline constexpr const char *kWeaponDepsCacheKind = "weapon-deps";

std::vector<uint8_t> encode_weapon_def(const WeaponDef &p_def);
Result<WeaponDef> decode_weapon_def(std::span<const uint8_t> p_blob);

using PartitionDigest = std::function<std::optional<Sha1>(std::string_view p_name)>;
PartitionDigest mount_partition_digest(const LevelMount &p_mount);
PartitionDigest library_partition_digest(const EbxLibrary &p_library);

struct WeaponCacheSources {
	const EbxResolver *level = nullptr;
	PartitionDigest level_digest;
	const EbxResolver *ui = nullptr;
	PartitionDigest ui_digest;
};

struct WeaponDependency {
	uint8_t resolver = 0;
	std::string name;
	bool operator==(const WeaponDependency &) const = default;
	auto operator<=>(const WeaponDependency &) const = default;
};

Result<WeaponDef> resolve_weapon_recorded(const WeaponCacheSources &p_sources, std::string_view p_variant, std::vector<WeaponDependency> &r_deps);

std::vector<uint8_t> encode_weapon_deps(const std::vector<WeaponDependency> &p_deps);
Result<std::vector<WeaponDependency>> decode_weapon_deps(std::span<const uint8_t> p_blob);

std::optional<CacheKey> weapon_deps_key(const WeaponCacheSources &p_sources, std::string_view p_variant);
std::optional<CacheKey> weapon_def_key(const WeaponCacheSources &p_sources, std::string_view p_variant, const std::vector<WeaponDependency> &p_deps);

struct CachedWeapon {
	WeaponDef def;
	CachedItem item;
	size_t dependencies = 0;
};

Result<CachedWeapon> resolve_weapon_cached(Cache *p_cache, const WeaponCacheSources &p_sources, std::string_view p_variant);

std::vector<std::string> weapon_variants_of(const EbxDb &p_db);

}
