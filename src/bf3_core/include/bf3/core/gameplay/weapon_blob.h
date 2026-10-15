#pragma once
// A resolved WeaponDef as a flat cache blob (plan §2.4 step 3: "resolved WeaponDef/VehicleDef/ModeConstants
// blobs"), and the cached resolver the game and `bf3dump prepare` use.
//
// Blob ("OBF3WDEF", kWeaponBlobFormatVersion): every WeaponDef field in declaration order, little endian:
// floats as f32, ints as i32/u32, sizes as u64, bools as one 0/1 byte, enums as their stored byte, strings as
// u32 length + bytes (at most 64 KiB each), vectors as u64 count + elements. decode_weapon_def checks every
// length against the bytes left and refuses trailing bytes, so encode(decode(b)) == b for every blob it accepts.
//
// Cache keys (cache_key.h). The sources of a WeaponDef are every EBX partition the resolver read (the variant,
// its blueprint, firing data, projectiles, sway, modifiers, the UI metadata), which are only known after a
// resolution. So a variant has two entries:
//   kind "weapon-deps": key = (SHA1 of the variant partition, converter version, engine, format,
//                       params "variant=<name>;ui=<0|1>") -> the sorted list of partitions read (with a
//                       resolver tag: 0 = level, 1 = UI);
//   kind "weapon":      key = (the SHA1s of those partitions in list order, a zero digest for a name that was
//                       looked up and absent, ..., params "variant=<name>;deps=<SHA-256 of the list>") -> the
//                       blob.
// A warm lookup reads the list, takes each partition's current manifest SHA1 from the mount (no payload read)
// and gets the blob. A patched partition changes the weapon key (a miss, then the list is rewritten when it
// changed); a patched variant partition changes both keys.

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

inline constexpr uint32_t kWeaponBlobFormatVersion = 1;
// Bump when resolve_weapon's output changes for the same partitions.
inline constexpr uint32_t kWeaponConverterVersion = 1;
inline constexpr const char *kWeaponCacheKind = "weapon";
inline constexpr const char *kWeaponDepsCacheKind = "weapon-deps";

std::vector<uint8_t> encode_weapon_def(const WeaponDef &p_def);
Result<WeaponDef> decode_weapon_def(std::span<const uint8_t> p_blob);

// The manifest SHA1 of a partition by name (nullopt when the resolver's superbundles do not hold it).
using PartitionDigest = std::function<std::optional<Sha1>(std::string_view p_name)>;
PartitionDigest mount_partition_digest(const LevelMount &p_mount);
PartitionDigest library_partition_digest(const EbxLibrary &p_library);

struct WeaponCacheSources {
	const EbxResolver *level = nullptr; // holds the weapon partitions (level EbxDb or EbxLibrary)
	PartitionDigest level_digest;
	const EbxResolver *ui = nullptr; // optional: ui/uiweaponmetadata for the name SID
	PartitionDigest ui_digest;
};

// One recorded dependency of a resolution.
struct WeaponDependency {
	uint8_t resolver = 0; // 0 level, 1 UI
	std::string name; // lowercase partition name
	bool operator==(const WeaponDependency &) const = default;
	auto operator<=>(const WeaponDependency &) const = default;
};

// resolve_weapon through resolvers that record every partition looked up (found or absent), sorted.
Result<WeaponDef> resolve_weapon_recorded(const WeaponCacheSources &p_sources, std::string_view p_variant, std::vector<WeaponDependency> &r_deps);

std::vector<uint8_t> encode_weapon_deps(const std::vector<WeaponDependency> &p_deps);
Result<std::vector<WeaponDependency>> decode_weapon_deps(std::span<const uint8_t> p_blob);

// The two keys (nullopt when the variant partition has no SHA1).
std::optional<CacheKey> weapon_deps_key(const WeaponCacheSources &p_sources, std::string_view p_variant);
std::optional<CacheKey> weapon_def_key(const WeaponCacheSources &p_sources, std::string_view p_variant, const std::vector<WeaponDependency> &p_deps);

struct CachedWeapon {
	WeaponDef def;
	CachedItem item; // of the "weapon" entry
	size_t dependencies = 0;
};

// The variant's WeaponDef from p_cache (may be null), or resolved and stored. Fails when the variant does not
// resolve.
Result<CachedWeapon> resolve_weapon_cached(Cache *p_cache, const WeaponCacheSources &p_sources, std::string_view p_variant);

// The SoldierWeaponUnlockAsset partitions under weapons/ of any resolver that can list its partitions
// (an EbxDb), sorted; the same set list_weapon_variants gives for a library.
std::vector<std::string> weapon_variants_of(const EbxDb &p_db);

} // namespace bf3::core::gameplay
