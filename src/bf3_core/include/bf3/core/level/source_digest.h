#pragma once
// The source digest of a loaded level x mode (plan §2.7 "source_digest"; docs/design/m1.md §5.5): one SHA-256 over
// the sorted (bundle name, bundle digest) of every bundle the mode mounts (LoadedLevel::bundles: the level bundle,
// its settings bundles, the included sub-worlds), where a bundle digest hashes its manifest entries
// (ebx name, sha1), (res name, resType, resMeta, sha1), (chunk id, sha1, range, tail-only), each list sorted
// (resMeta and the chunk range change how a payload decodes without changing its SHA1). Nothing is re-hashed from
// the payloads and nothing is written: the SHA1s are the ones the manifests carry (LevelMount::read_manifest); an
// entry stored inline in a CAS manifest with a zero SHA1 is hashed by its inline bytes, which the manifest holds.
//
// M1 uses it as the key of the static world's "jolt-world" cache entry (bf3_sim static_world.h): equal digests
// mean the soup would be built from the same bytes. M3 sends it in MatchInfo/Ready (the data-identity handshake).
// A bundle whose manifest has no usable SHA1 table (a delta bundle before reconstruction, plan §2.7 "What it is
// not") makes the digest unavailable: callers then take the cold path, whose per-tile keys need no digest.

#include "bf3/core/cache/digest.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <optional>

namespace bf3::core::level {

class LoadedLevel;

struct SourceDigest {
	std::optional<Sha256Digest> digest; // nullopt: some bundle has no SHA1 table (see above)
	size_t bundles = 0;
	size_t entries = 0; // manifest entries hashed
	double seconds = 0.0;
};

// Fails only when a bundle manifest cannot be read.
Result<SourceDigest> level_source_digest(const LoadedLevel &p_level);

} // namespace bf3::core::level
