#pragma once

#include "bf3/core/cache/digest.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <optional>

namespace bf3::core::level {

class LoadedLevel;

struct SourceDigest {
	std::optional<Sha256Digest> digest;
	size_t bundles = 0;
	size_t entries = 0;
	double seconds = 0.0;
};

Result<SourceDigest> level_source_digest(const LoadedLevel &p_level);

}
