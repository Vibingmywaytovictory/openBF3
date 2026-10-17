#pragma once
// The places a level x mode's gameplay points at (spike S1, docs/design/s1.md §4, §5): the combat areas (where a
// soldier may be), the capture points (flags and HQs) and the Rush MCOMs, with world positions, read from the
// traversal (bf3_core level/world.h). Godot-free. The spawn points are level/spawn_points.h's.
//
// Data, verified with bf3dump level / dump on 2026-10-15 (docs/design/s1.md §4):
//   combat area  a CombatAreaTriggerEntityData of the mode's game-mode prefab (Metro and Caspian CQ: two, in
//                gameplay/gamemodes/conquest, Team1 and Team2, at the identity transform) has no shape of its own: a
//                LinkConnection from the trigger to the prefab's interface (field ids -1793770131 / -1793770130 on the
//                interface side), and in the mode's SubWorldData (e.g. levels/mp_subway/conquest_small) a
//                LinkConnection from the prefab's ReferenceObjectData field to a VolumeVectorShapeData placed in the
//                mode's logic layer, whose Points (XZ polygon, closed, Height 0 = no vertical limit) are the area.
//                Metro CQL: 4 and 5 points; Caspian CQL: 8 and 12 points (x -627..690, z -583..868); Kharg CQL: 8
//                and 7 points (x -1532..1097, z -1795..1345).
//   capture point every CapturePointEntityData placement (ObjectKind::CapturePoint): an HQ when CapturableType is
//                CTUnableToChangeTeam (gameplay ModeDef's CapturePointDef::is_hq). Metro CQL/CQS: 2 HQs + 3 flags;
//                Caspian CQL: 2 HQs + 5 flags, CQS: 6 placements; TDM, SQDM, Rush, SR: none.
//   MCOM         every rushcrate scope (gameplay/level_setups/components/rushcrate) placed by the mode; its position
//                is the transform of the scope's GameInteractionEntityData with InteractionType IET_RushCrateArm
//                (Metro RushLarge0: 10 crates, 20 GameInteractionEntityData = arm + disarm per crate).
// Bounds: every array and connection the read scans and every name it compares is charged to the World's
// WorkBudget (budget.h) before it is done, like the other helpers; polygons over kMaxCombatAreaPoints points and more
// than kMaxCombatAreas areas are errors. Hostile data fails with an error.

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
// combat_area_tiles' work bound: candidate tiles x polygon edge samples, summed over the areas (a retail area is a
// few hundred tiles x a few hundred samples, about 10^5; review of revision 2: the tile test is tiles x edges).
inline constexpr uint64_t kMaxCombatAreaTileWork = uint64_t(1) << 26;

struct CombatAreaDef {
	uint32_t trigger = 0; // World::objects index of the CombatAreaTriggerEntityData
	uint32_t shape = 0; // World::objects index of the VolumeVectorShapeData
	int team = 0; // the trigger's Team (TeamId; 0 = every team)
	std::vector<std::array<float, 2>> polygon; // world XZ, in the shape's point order (closed)
	float height = 0.0f; // VolumeVectorShapeData.Height (0 on every retail combat area: no vertical limit)
};

struct ModeObjectiveDef {
	enum class Kind : uint8_t { Flag = 0, Hq = 1, Mcom = 2 };
	Kind kind = Kind::Flag;
	uint32_t object = 0; // World::objects index of the CapturePointEntityData / the arm GameInteractionEntityData
	Vec3 position{}; // its world transform's translation
	int team = 0; // InitialOwner (capture points) or 0
	std::string scope_label; // the prefab partition it sits in, for reports
};

struct ModeAreas {
	std::vector<CombatAreaDef> combat_areas; // trigger order
	std::vector<ModeObjectiveDef> objectives; // object order
	// Triggers whose Geometry chain runs out of links (an interface field the instantiating scope never links): they
	// watch no area and add no tiles (retail: Metro RL, open question 7 of docs/design/s1.md). A chain that reaches a
	// target of another type is still an error.
	std::vector<uint32_t> unwired_triggers;
};

// Reads the combat areas and objectives of a traversed level x mode. World is not thread-safe: caller's thread.
Result<ModeAreas> read_mode_areas(const World &p_world);

// The water surfaces (WaterPlane, collision_soup.h; docs/design/s1.md §5.2 "Deep water"): every WaterEntityData
// placement's Asset partition holds LakeData shapes (VisualVectorShapeData: Points, closed). Verified on 2026-10-15:
// Kharg's levels/mp_018/terrain/terrain_mp018.water.mesh has one LakeData, a closed 8.8 km square at y 88.47, under a
// WaterEntityData at the identity transform (whose own transform therefore does not give the height); Metro's park
// pond is at y 62.56 (14 points), Caspian's one lake at y 67.26 (9 points, under the terrain).
// Every lake of the traversed level x mode, charged to the World's budget (points over kMaxCombatAreaPoints, a
// non-finite point or more than kMaxCombatAreas lakes fail). A WaterEntityData whose Asset does not resolve is
// skipped (no plane: the water rule is off there).
Result<std::vector<WaterPlane>> read_water_planes(const World &p_world);


// The 64 m tiles (the soup's tile key: floor(x / 64), floor(z / 64)) that any polygon of p_areas, grown by p_margin
// metres, touches; sorted by (x, z), unique. The work (each area's candidate tiles x its edge samples) is summed
// before the loop and refused past kMaxCombatAreaTileWork, so a hostile polygon fails instead of running for hours.
Result<std::vector<std::array<int32_t, 2>>> combat_area_tiles(std::span<const CombatAreaDef> p_areas, float p_margin);

// The tiles within p_margin metres of a point (the square [x - m, x + m] x [z - m, z + m]: at most 4 tiles at the
// default 32 m margin); non-finite points give none.
std::vector<std::array<int32_t, 2>> point_tiles(const Vec3 &p_point, float p_margin);

// The union of combat_area_tiles over every MP mode of a level (gameplay level_modes: MP_Subway, MP_007 and MP_018
// list 7 each), each mode loaded and traversed on the calling thread (about 0.2-0.4 s per mode), plus each mode's
// areas for the reports. The plan bakes navigation over this union so that the modes share tiles (§3.6).
// The region also takes point_tiles(p, p_margin) of every objective (flags, HQs, MCOMs) and every AlternateSpawn
// point (read_level_spawns) of every mode, so a gameplay point outside every combat polygon is still baked and G2
// judges it for a nav reason, never for a region reason (review of revision 2; Caspian CQL/RL/TDM points checked
// inside the polygons, the other modes are counted here). The same tiles are the terrain cover (docs §5.2).
struct LevelCombatTiles {
	std::vector<std::string> modes;
	std::vector<ModeAreas> areas; // per mode
	std::vector<std::array<int32_t, 2>> tiles; // union of the polygon tiles and the point tiles, sorted, unique
	size_t polygon_tiles = 0; // tiles the combat polygons (+ margin) touch, before the points
	size_t point_only_tiles = 0; // tiles only a point added (reported; 0 expected on retail data)
	size_t points = 0; // objectives + spawn points over every mode
	size_t points_outside_areas = 0; // of those, outside every combat polygon of their own mode (reported)
	// The water surfaces of the level (read_water_planes of the first mode that has any; the lakes are level
	// data, the same in every mode). The nav bake's water rule (NavBakeOptions::water; TC) uses them.
	std::vector<WaterPlane> water;
	double seconds = 0.0;
};
Result<LevelCombatTiles> level_combat_tiles(std::shared_ptr<const Install> p_install, std::string_view p_level_id, float p_margin);

} // namespace bf3::core::level
