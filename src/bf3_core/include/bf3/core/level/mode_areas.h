#pragma once

#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/world.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Install;
}

namespace bf3::core::level {

inline constexpr size_t kMaxCombatAreaPoints = 4096;
inline constexpr size_t kMaxCombatAreas = 256;

inline constexpr uint64_t kMaxCombatAreaTileWork = uint64_t(1) << 26;

struct CombatAreaDef {
	uint32_t trigger = 0;
	uint32_t shape = 0;
	int team = 0;
	std::vector<std::array<float, 2>> polygon;
	float height = 0.0f;
};

struct ModeObjectiveDef {
	enum class Kind : uint8_t { Flag = 0, Hq = 1, Mcom = 2 };
	Kind kind = Kind::Flag;
	uint32_t object = 0;
	Vec3 position{};
	int team = 0;
	std::string scope_label;
};

struct ModeAreas {
	std::vector<CombatAreaDef> combat_areas;
	std::vector<ModeObjectiveDef> objectives;

	std::vector<uint32_t> unwired_triggers;
};

Result<ModeAreas> read_mode_areas(const World &p_world);

Result<std::vector<WaterPlane>> read_water_planes(const World &p_world);

Result<std::vector<std::array<int32_t, 2>>> combat_area_tiles(std::span<const CombatAreaDef> p_areas, float p_margin);

std::vector<std::array<int32_t, 2>> point_tiles(const Vec3 &p_point, float p_margin);

struct LevelCombatTiles {
	std::vector<std::string> modes;
	std::vector<ModeAreas> areas;
	std::vector<std::array<int32_t, 2>> tiles;
	size_t polygon_tiles = 0;
	size_t point_only_tiles = 0;
	size_t points = 0;
	size_t points_outside_areas = 0;

	std::vector<WaterPlane> water;
	double seconds = 0.0;
};
Result<LevelCombatTiles> level_combat_tiles(std::shared_ptr<const Install> p_install, std::string_view p_level_id, float p_margin);

}
