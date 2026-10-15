#pragma once
// Compiled EffectDefs through the derived-data cache (docs/design/m2a-vfx1.md §8; plan §2.4 step 3 "compiled
// EffectDefs (r2)"; README "Preparing the cache"). The pattern is the WeaponDef one (gameplay/weapon_blob.h): the
// sources of an effect (the blueprint, its EmitterDocuments, their base and child documents) are known only after a
// compile, so an effect has two entries:
//   kind "fx-deps":   key = (SHA1 of the blueprint partition, kFxConverterVersion, engine, kFxDepsFormatVersion,
//                     params "effect=<name>") -> the compile's source list (lowercase partition names, read order);
//   kind "fx-effect": key = (the SHA1s of those partitions in list order, a zero digest for an absent one,
//                     kFxConverterVersion, engine, kFxBlobFormatVersion, params "effect=<name>;deps=<SHA-256 of the
//                     list>") -> the EffectDef blob.
// A warm lookup reads the list, takes each partition's manifest SHA1 from the mount (no EBX parsed) and gets the
// blob. A patched emitter document changes the effect key; a patched blueprint changes both. The engine field is
// cache_engine_id() as for every kind, so an engine bump recompiles (cheap: about a millisecond per effect).
//
// Blob "OBF3FXEF" (kFxBlobFormatVersion): every EffectDef field in declaration order, little endian (floats f32, enums
// and bools one byte, strings u32 length + bytes, at most kMaxNameLength; vectors u32 count + elements). The decoder
// checks every count against the bytes left and the fx_types.h bounds before it allocates, checks every enum, index
// (component -> emitter, slot -> evaluator, child -> emitter, gradient range) and float (finite, |x| <=
// kMaxAbsParameter), and refuses trailing bytes, so encode(decode(b)) == b for every blob it accepts.

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
// Bump when the compiler's output changes for the same partitions (any semantic change of docs §6).
inline constexpr uint32_t kFxConverterVersion = 1;
inline constexpr const char *kFxEffectCacheKind = "fx-effect";
inline constexpr const char *kFxDepsCacheKind = "fx-deps";

std::vector<uint8_t> encode_effect_def(const EffectDef &p_def);
core::Result<EffectDef> decode_effect_def(std::span<const uint8_t> p_blob);

std::vector<uint8_t> encode_effect_deps(const std::vector<std::string> &p_sources);
core::Result<std::vector<std::string>> decode_effect_deps(std::span<const uint8_t> p_blob);

// The manifest SHA1 of a partition by name (bf3::core::gameplay::mount_partition_digest / library_partition_digest).
using PartitionDigest = core::gameplay::PartitionDigest;

struct FxSources {
	const core::EbxResolver *db = nullptr;
	PartitionDigest digest;
};

// nullopt when the blueprint has no SHA1 (then the effect is compiled uncached).
std::optional<core::CacheKey> effect_deps_key(const FxSources &p_sources, std::string_view p_blueprint);
std::optional<core::CacheKey> effect_def_key(const FxSources &p_sources, std::string_view p_blueprint, const std::vector<std::string> &p_deps);

struct CachedEffect {
	EffectDef def;
	core::CachedItem item; // of the "fx-effect" entry
	size_t dependencies = 0;
};

// The blueprint's EffectDef from p_cache (may be null), or compiled and stored. Fails when the effect does not
// compile. Thread-safe across blueprints (the Cache and EbxLibrary are).
core::Result<CachedEffect> compile_effect_cached(core::Cache *p_cache, const FxSources &p_sources, std::string_view p_blueprint,
		core::ConvertTotals *r_totals = nullptr);

} // namespace bf3::fx
