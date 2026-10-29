#pragma once

#include "bf3/core/level/mode_areas.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::core {
class Cache;
class Install;
}

namespace bf3::core::level {

class LoadedLevel;

inline constexpr const char *kCombatTilesCacheKind = "combat-tiles";
inline constexpr uint32_t kCombatTilesFormatVersion = 1;

struct CombatTilesLoad {
	LevelCombatTiles tiles;
	bool cache_hit = false;
	bool cached = false;
	double seconds_key = 0.0;
	double seconds = 0.0;
};

std::vector<uint8_t> encode_combat_tiles(const LevelCombatTiles &p_tiles);
Result<LevelCombatTiles> decode_combat_tiles(std::span<const uint8_t> p_bytes);

Result<CombatTilesLoad> load_level_combat_tiles(
		Cache *p_cache, std::shared_ptr<const Install> p_install, const LoadedLevel &p_level, float p_margin);

}
