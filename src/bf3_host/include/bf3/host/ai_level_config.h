#pragma once

#include "bf3/ai/main/ai_script.h"
#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/result.h"
#include "bf3/host/bot_fill.h"

#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Cache;
}

namespace bf3::host {

inline constexpr const char *kAiAreasCacheKind = "ai-areas";
inline constexpr uint32_t kAiAreasConverterVersion = 2;

struct CachedAiAreas {
	std::string text;
	core::CacheOutcome outcome = core::CacheOutcome::Uncached;
};

std::vector<ai::StrategicAreaDef> derive_strategic_areas(const BotLevel &p_level);
core::Result<CachedAiAreas> cached_strategic_areas(
		core::Cache *p_cache, const core::Sha256Digest &p_install, std::string_view p_level, const BotLevel &p_bots);

}
