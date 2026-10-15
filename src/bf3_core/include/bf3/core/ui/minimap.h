#pragma once
// BF3's minimap read from the install (plan §3.8, D16, §7.8; research/topics/gap-r1-ui-hud-minimap-localisation.md
// §2; docs/design/ui-minimap-icons.md §5). Godot-free; the Godot side draws it (bf3_godot minimap_*.h).
//
// Where the data is (verified 2026-10-15 with bf3dump, design §4):
//   * levels/<l>/<l> (the LevelData partition) -> LevelDescription.Components -> UILevelDescriptionComponent
//     .MinimapData (UIMinimapData): WorldCenter (Vec2: Frostbite x, z), WorldSize, WorldRotation (0 on all 29
//     levels), WorldRange, AirRadarRange, DetailTexture (TextureAsset: the L8 "_satellite"), DetailTextureTint,
//     Detail / Vegetation (UIMinimapDistanceFieldParams -> UIDistanceFieldAsset -> TextureAsset "..._win32_sdf"),
//     CombatArea* (alpha threshold 0.496 on all 29, distance scale 150, or 5 on three XP2 levels), FadeTexture,
//     UseStreamingMinimap (true on the four XP2 levels, whose floors are separate distance fields).
//     The levels/<l>/<l>/description_win32 copy holds defaults only (WorldCenter 0,0, WorldSize 4096): never read it
//     for geometry.
//   * The mode's combat areas: each sub-world the mode includes places UICombatAreaEntityData objects whose Asset is a
//     VeniceUICombatAreaAsset (levels/<l>/ui/minimap/ca*: Prefix, DistanceField, SurroundingDistanceField, each a
//     UIDistanceFieldAsset -> TextureAsset -> DxTexture L8 128x128). Metro TDM: one (catdm -> dftdm), Metro CQS: two
//     (cacs1, cacs2). 455 such assets on 25 levels (none on the four XP2 Close Quarters levels), 498 distinct
//     distance fields, 92 null SurroundingDistanceField references.
//   * The objectives: the traversal's CapturePoint placements (CapturePointEntityData; HQs have CapturableType
//     CTUnableToChangeTeam).
//
// Mapping (research §2, re-checked design §4): image row 0 is the top, both axes mirrored against the naive mapping:
//   u = 0.5 - (x - WorldCenter.x) / WorldSize,   v = 0.5 - (z - WorldCenter.y) / WorldSize.
// Inside the combat area: the SDF texel / 255 < CombatAreaAlphaThreshold.
//
// Hostile input: every reader charges a level::WorkBudget before the work (as the level code does); images are
// refused past kMaxMinimapImageSide and combat areas past kMaxCombatAreas, so a load is bounded.

#include "bf3/core/ebx_db.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/result.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Install;
class LevelMount;
} // namespace bf3::core

namespace bf3::core::level {
class LoadedLevel;
class World;
} // namespace bf3::core::level

namespace bf3::core::ui {

// ---- limits (design §9) -------------------------------------------------------------------------------------------
inline constexpr uint32_t kMaxMinimapImageSide = 4096; // retail satellites 1024-2048, SDFs 128-1024
inline constexpr size_t kMaxCombatAreas = 64; // per level x mode (Metro TDM 1, Metro CQS 2; Metro's a1-a5 / d1-d5 suggest one per Rush stage and side)
inline constexpr size_t kMaxObjectives = 64; // capture points per level x mode
inline constexpr float kResearchCombatAreaThreshold = 0.496f; // UIMinimapData.CombatAreaAlphaThreshold, all 29 levels

// ---- UIMinimapData -------------------------------------------------------------------------------------------------

// UIMinimapDistanceFieldParams (Detail, Vegetation).
struct MinimapFieldParams {
	std::string distance_field_asset; // UIDistanceFieldAsset partition; empty when null (XP2)
	std::string texture; // its DistanceFieldTexture: the TextureAsset partition = the DxTexture res name
	float alpha_threshold = 0.5f;
	float distance_scale = 1.0f;
	float outline_inner = 0.0f;
	float outline_outer = 0.0f;
	std::array<float, 4> color_tint{ 1, 1, 1, 1 }; // x, y, z, w as in the data
	std::array<float, 4> outline_color{ 0, 0, 0, 0 };
};

struct MinimapData {
	std::string level_id; // MP_Subway
	std::string partition; // levels/mp_subway/mp_subway (the LevelData partition it was read from)
	std::array<float, 2> world_center{}; // WorldCenter (x, y) = Frostbite (x, z), metres
	float world_size = 0.0f; // WorldSize, metres across the image
	float world_rotation = 0.0f; // WorldRotation (0 on every retail level; a non-zero value is refused, design §5.2)
	float world_range = 0.0f;
	float air_radar_range = 0.0f;
	std::string satellite; // DetailTexture: TextureAsset partition = DxTexture res name (L8)
	std::array<float, 4> detail_texture_tint{ 1, 1, 1, 1 };
	MinimapFieldParams detail;
	MinimapFieldParams vegetation;
	float combat_area_alpha_threshold = kResearchCombatAreaThreshold;
	float combat_area_distance_scale = 0.0f;
	float combat_area_scale = 1.0f;
	float combat_area_fade_speed = 1.0f;
	std::array<float, 4> combat_area_color{ 1, 1, 1, 1 };
	std::string combat_area_multiply_texture; // ui/hud/minimap/combatarea (a pattern), empty when null
	std::array<float, 2> combat_area_multiply_wrap{ 1, 1 };
	std::string fade_texture; // ui/hud/minimap/minimapmask
	std::string air_radar_fade_texture;
	bool use_combat_area_texture = true;
	bool use_streaming_minimap = false; // the XP2 floors (not drawn in this wave, design §14)
};

// ---- mapping (inline: the contract both the readers and the Godot side use) ---------------------------------------

struct MinimapMapping {
	float center_x = 0.0f; // WorldCenter.x
	float center_z = 0.0f; // WorldCenter.y
	float size = 1.0f; // WorldSize
};

inline MinimapMapping minimap_mapping(const MinimapData &p_data) {
	return { p_data.world_center[0], p_data.world_center[1], p_data.world_size };
}

struct MinimapUv {
	float u = 0.0f; // 0 = left edge of the image
	float v = 0.0f; // 0 = top row
};

// The mapping, research §2: both axes mirrored.
constexpr MinimapUv world_to_minimap_uv(const MinimapMapping &p_m, float p_x, float p_z) {
	return { 0.5f - (p_x - p_m.center_x) / p_m.size, 0.5f - (p_z - p_m.center_z) / p_m.size };
}
// The inverse: world (x, z) of an image point.
constexpr std::array<float, 2> minimap_uv_to_world(const MinimapMapping &p_m, MinimapUv p_uv) {
	return { p_m.center_x + (0.5f - p_uv.u) * p_m.size, p_m.center_z + (0.5f - p_uv.v) * p_m.size };
}

// The 8 sign / transpose alternatives the orientation gates compare against (design §5.3). Bit 0: u = +(x - cx)/S
// instead of -, bit 1: v = +(z - cz)/S instead of -, bit 2: u and v swapped after the signs. Variant 0 is the
// research mapping (world_to_minimap_uv).
inline constexpr uint8_t kMinimapMappingVariants = 8;
constexpr MinimapUv world_to_minimap_uv_variant(const MinimapMapping &p_m, float p_x, float p_z, uint8_t p_variant) {
	const float a = (p_x - p_m.center_x) / p_m.size;
	const float b = (p_z - p_m.center_z) / p_m.size;
	const float u = (p_variant & 1) ? 0.5f + a : 0.5f - a;
	const float v = (p_variant & 2) ? 0.5f + b : 0.5f - b;
	return (p_variant & 4) ? MinimapUv{ v, u } : MinimapUv{ u, v };
}

// ---- the on-screen view (design §5.5) -----------------------------------------------------------------------------
// Screen coordinates are Godot's canvas: x right, y down, pixels. The player sits at `center`. Heading is the sim's
// yaw (radians; 0 looks down +Z, pi/2 down +X: sim_types.h). On the image, +Z is up and +X is left (the mirrored
// mapping), so a north-up map shows the view direction at the clockwise screen angle -heading; with rotate set the
// map turns clockwise by +heading about the player, so the view direction always points up.
struct MinimapView {
	float center_x = 0.0f; // screen position of the player, pixels
	float center_y = 0.0f;
	float pixels_per_metre = 1.0f;
	float player_x = 0.0f; // world position of the player (Frostbite x, z)
	float player_z = 0.0f;
	float heading = 0.0f; // sim yaw, radians
	bool rotate = true; // BF3's in-game minimap turns with the view (UIMinimapCompData.PlayerIsCenter)
};

struct ScreenPoint {
	float x = 0.0f;
	float y = 0.0f;
};

// Screen position of world point (x, z).
inline ScreenPoint minimap_project(const MinimapView &p_view, float p_x, float p_z) {
	// North-up offset: image u grows with -x, image v (screen down) grows with -z.
	const float sx = -(p_x - p_view.player_x) * p_view.pixels_per_metre;
	const float sy = -(p_z - p_view.player_z) * p_view.pixels_per_metre;
	if (!p_view.rotate) {
		return { p_view.center_x + sx, p_view.center_y + sy };
	}
	// Clockwise by +heading in y-down screen space.
	const float c = std::cos(p_view.heading);
	const float s = std::sin(p_view.heading);
	return { p_view.center_x + sx * c - sy * s, p_view.center_y + sx * s + sy * c };
}

// World (x, z) under a screen point (the inverse of minimap_project).
inline std::array<float, 2> minimap_unproject(const MinimapView &p_view, ScreenPoint p_screen) {
	float sx = p_screen.x - p_view.center_x;
	float sy = p_screen.y - p_view.center_y;
	if (p_view.rotate) {
		const float c = std::cos(p_view.heading);
		const float s = std::sin(p_view.heading);
		const float rx = sx * c + sy * s;
		const float ry = -sx * s + sy * c;
		sx = rx;
		sy = ry;
	}
	return { p_view.player_x - sx / p_view.pixels_per_metre, p_view.player_z - sy / p_view.pixels_per_metre };
}

// The clockwise screen angle of the player marker's arrow (drawn pointing up at angle 0).
inline float minimap_marker_angle(const MinimapView &p_view) {
	return p_view.rotate ? 0.0f : -p_view.heading;
}

// ---- the UV read-back the in-game check uses (design §10.4, gate G9) -----------------------------------------------
// With the shader's debug_uv uniform set, the map item writes the texture coordinate it sampled into the pixel instead
// of the satellite colour: u and v quantised to 12 bits each (1/4095: half a texel of a 2048² satellite), packed into
// R, G, B with alpha 1 (R = u >> 4, G = (u & 15) << 4 | v >> 8, B = v & 255). The shader in minimap_control.h does the
// same packing; ui2.contract checks the round trip. The check reads these pixels back from the viewport image and
// compares them with minimap_expected_uv of the same pixel, so the satellite's placement (draw_set_transform, the
// source rect, the rotation, the mirror) is measured, not assumed.
inline constexpr uint32_t kMinimapDebugUvSteps = 4095;
struct MinimapDebugUvPixel {
	uint8_t r = 0;
	uint8_t g = 0;
	uint8_t b = 0;
};
inline MinimapDebugUvPixel encode_minimap_debug_uv(MinimapUv p_uv) {
	const auto quantise = [](float p_x) -> uint32_t {
		if (!std::isfinite(p_x) || p_x <= 0.0f) {
			return 0;
		}
		if (p_x >= 1.0f) {
			return kMinimapDebugUvSteps;
		}
		return uint32_t(std::floor(p_x * float(kMinimapDebugUvSteps) + 0.5f));
	};
	const uint32_t qu = quantise(p_uv.u);
	const uint32_t qv = quantise(p_uv.v);
	return { uint8_t(qu >> 4), uint8_t(((qu & 15u) << 4) | (qv >> 8)), uint8_t(qv & 255u) };
}
inline MinimapUv decode_minimap_debug_uv(MinimapDebugUvPixel p_pixel) {
	const uint32_t qu = (uint32_t(p_pixel.r) << 4) | (uint32_t(p_pixel.g) >> 4);
	const uint32_t qv = ((uint32_t(p_pixel.g) & 15u) << 8) | uint32_t(p_pixel.b);
	return { float(qu) / float(kMinimapDebugUvSteps), float(qv) / float(kMinimapDebugUvSteps) };
}
// The satellite UV that must be under screen point p_screen (control-local canvas units) when the map is drawn right:
// the world point minimap_unproject gives, through the mapping (variant 0 = world_to_minimap_uv). The check compares
// the decoded read-back against this for variant 0 (within kMinimapUvCheckTexels) and, as a control, for the other
// seven (each must miss by more than kMinimapUvMirrorTexels at some sample).
inline MinimapUv minimap_expected_uv(const MinimapView &p_view, const MinimapMapping &p_mapping, ScreenPoint p_screen, uint8_t p_variant = 0) {
	const std::array<float, 2> w = minimap_unproject(p_view, p_screen);
	return world_to_minimap_uv_variant(p_mapping, w[0], w[1], p_variant);
}
inline constexpr float kMinimapUvCheckTexels = 1.0f; // pass: |decoded - expected| <= 1 satellite texel per axis (the
                                                       // 12-bit packing costs at most 0.25 texel on a 2048² image)
inline constexpr float kMinimapUvMirrorTexels = 10.0f; // the 7 other variants must miss by more than this somewhere
// The larger per-axis error in texels of a side p_side image.
inline float minimap_uv_error_texels(MinimapUv p_a, MinimapUv p_b, uint32_t p_side) {
	const float du = std::fabs(p_a.u - p_b.u) * float(p_side);
	const float dv = std::fabs(p_a.v - p_b.v) * float(p_side);
	return du > dv ? du : dv;
}

// ---- combat areas and objectives ----------------------------------------------------------------------------------

// One VeniceUICombatAreaAsset the mode places (through a UICombatAreaEntityData).
struct CombatAreaMask {
	std::string asset; // levels/mp_subway/ui/minimap/cacs1
	std::string prefix; // Prefix as written ("cs", "tdm", "caRA1", ...: not a reliable mode key, design §5.4)
	std::string distance_field_asset; // UIDistanceFieldAsset partition
	std::string texture; // its TextureAsset = DxTexture res name (L8 128x128 on retail)
	std::string surrounding_asset; // SurroundingDistanceField; empty when null
	std::string surrounding_texture;
	std::string entity; // the placing UICombatAreaEntityData, "<partition>#<index>"
};

enum class ObjectiveKind : uint8_t {
	Flag, // CapturePointEntityData that can change owner
	Hq, // CapturableType CTUnableToChangeTeam
};

struct MinimapObjective {
	ObjectiveKind kind = ObjectiveKind::Flag;
	std::array<float, 3> position{}; // Frostbite world metres (the placement's translation)
	float yaw = 0.0f; // of the placement, sim convention
	int32_t initial_owner = 0; // InitialOwnerTeam (0 neutral, 1, 2), PropertyConnections applied
	std::string label_sid; // LocationNameSid (e.g. ID_H_US_A) when set
	std::string location_sid; // LocationTextSid (e.g. ID_H_MAP_MP_SUBWAY_CAFE) when set
	std::string entity; // "<partition>#<index>"
};

// Everything the minimap of one level x mode needs, without pixels.
struct MinimapLevel {
	MinimapData data;
	std::string mode; // TeamDeathMatch0
	std::vector<CombatAreaMask> combat_areas; // traversal order
	std::vector<MinimapObjective> objectives; // traversal order
	size_t work = 0; // budget units this read spent
};

// ---- readers (bodies: the implementation wave, design §15) ---------------------------------------------------------

// UIMinimapData of the level's own LevelData (p_level_root: the LevelData instance of levels/<l>/<l>). Fails when the
// level has no UILevelDescriptionComponent with MinimapData, or a field is out of range (WorldSize <= 0 or not finite,
// WorldRotation != 0, a threshold outside [0, 1]).
Result<MinimapData> read_minimap_data(const EbxResolver &p_db, const EbxInstance &p_level_root, std::string_view p_level_id,
		level::WorkBudget &r_budget);
// One VeniceUICombatAreaAsset partition and its two distance fields.
Result<CombatAreaMask> read_combat_area(const EbxResolver &p_db, std::string_view p_asset_partition, level::WorkBudget &r_budget);
// The minimap of a loaded and traversed level x mode: MinimapData, the UICombatAreaEntityData placements' assets, the
// CapturePoint placements. Charges the level's budget (p_world.budget).
Result<MinimapLevel> read_minimap_level(const level::LoadedLevel &p_level, const level::World &p_world);

struct MinimapLoadOptions {
	// Partition parse lanes (level::LoadOptions::threads); 0 = all cores. The in-game HUD passes
	// kMinimapHudLoaderThreads (plan §2.3: loader work stays off the cores the frame and the 60 Hz server need);
	// bf3dump and the install tests pass 0.
	unsigned threads = 0;
};
inline constexpr unsigned kMinimapHudLoaderThreads = 2;
// load_level + World::traverse + read_minimap_level (the minimap node's loader thread, bf3dump minimap).
Result<MinimapLevel> load_minimap_level(std::shared_ptr<const Install> p_install, std::string_view p_level_id, std::string_view p_mode,
		const MinimapLoadOptions &p_options = {});

// ---- images ----------------------------------------------------------------------------------------------------------

// One L8 image, row 0 at the top (v = 0).
struct L8Image {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> texels; // width * height

	bool empty() const { return texels.empty(); }
	// Nearest texel at (u, v), clamped to the edges; 255 (outside any combat area) for an empty image or a non-finite
	// coordinate, so hostile positions never reach the float -> integer conversion.
	uint8_t at_uv(MinimapUv p_uv) const {
		if (empty() || !std::isfinite(p_uv.u) || !std::isfinite(p_uv.v)) {
			return 255;
		}
		const float fx = std::floor(p_uv.u * float(width));
		const float fy = std::floor(p_uv.v * float(height));
		const uint32_t x = fx < 0.0f ? 0u : (fx >= float(width) ? width - 1 : uint32_t(fx));
		const uint32_t y = fy < 0.0f ? 0u : (fy >= float(height) ? height - 1 : uint32_t(fy));
		return texels[size_t(y) * width + x];
	}
};

// Inside the combat area (research §2): below the threshold.
constexpr bool sdf_inside(uint8_t p_texel, float p_threshold) {
	return float(p_texel) / 255.0f < p_threshold;
}

// The pixels of one level x mode's minimap (textures are decoded from the install at every load, never cached:
// plan §2.4). combat_areas and surroundings are parallel to MinimapLevel::combat_areas; an absent field is empty.
struct MinimapImages {
	L8Image satellite;
	L8Image detail; // empty on the XP2 levels
	L8Image vegetation;
	std::vector<L8Image> combat_areas;
	std::vector<L8Image> surroundings;
};

// A DxTexture res that must be L8, 2D, one face, at most kMaxMinimapImageSide a side: its largest mip.
Result<L8Image> load_l8_texture(const LevelMount &p_mount, std::string_view p_res_name);
// The same checks on a parsed header and its whole chunk (load_l8_texture's decode step; the unit and fuzz tests
// feed it synthetic and mutated headers). p_name is for messages.
Result<L8Image> decode_l8_texture(const res::DxTexture &p_texture, std::span<const uint8_t> p_chunk, std::string_view p_name);
// The level superbundle mounted read-only with every bundle indexed: the satellite, the fields and the combat-area
// textures ship in the level bundle and its sub-world bundles (the mount load_minimap_images reads from).
Result<LevelMount> mount_minimap_level(const Install &p_install, std::string_view p_level_id);
Result<MinimapImages> load_minimap_images(const LevelMount &p_mount, const MinimapLevel &p_level);
// Box-filtered mips down to 1x1, largest first (the satellite ships one mip; the Godot upload wants a chain so the
// 2048² image does not alias at 300 px). p_image is mip 0.
std::vector<L8Image> l8_mip_chain(const L8Image &p_image);

// ---- gates (design §1: G3..G5 are computed with these) ---------------------------------------------------------------

// Union of the masks: inside any of them (all must share one size; an empty image is skipped).
Result<std::vector<bool>> combat_area_union(std::span<const L8Image> p_masks, float p_threshold);
// IoU of a mask and an axis-aligned world box rasterised at the mask's texel centres through one mapping variant.
double mask_box_iou(const std::vector<bool> &p_mask, uint32_t p_side, const MinimapMapping &p_mapping, uint8_t p_variant,
		std::array<float, 4> p_box_x0_x1_z0_z1);
// The same against a closed world polygon (x, z) (even-odd rule): the gameplay combat area's VolumeVectorShapeData
// (design §5.3: Metro TDM's 23-point area gives IoU 1.000 through variant 0 and 0.000 through the other seven).
// Cost side² x points: refused (NaN) past kMaxIouSide or kMaxAreaPolygonPoints.
double mask_polygon_iou(const std::vector<bool> &p_mask, uint32_t p_side, const MinimapMapping &p_mapping, uint8_t p_variant,
		std::span<const std::array<float, 2>> p_polygon);

inline constexpr uint32_t kMaxIouSide = 1024; // retail masks 128²
inline constexpr size_t kMaxAreaPolygonPoints = 1024; // retail: Metro TDM's gameplay area has 23

// G5's reference polygon: the (x, z) of the Points of the one ObjectKind::VolumeVectorShape placement of p_world whose
// entity lives in partition p_partition (Metro TDM: levels/mp_subway/teamdeathmatch/layer0_teamdeathmatch_logic, whose
// only VolumeVectorShapeData, #104 in the traversal, c304182e-..., is the area the TDM sub-world links to the
// CombatAreaTriggerEntityData of gameplay/gamemodes/deathmatch; design §4.2), each point through the placed transform
// (identity on Metro). Fails on 0 or more than one such placement, a non-finite point, fewer than 3 points or more than
// kMaxAreaPolygonPoints; charges p_world's budget per point.
Result<std::vector<std::array<float, 2>>> read_area_polygon(const level::LoadedLevel &p_level, const level::World &p_world,
		std::string_view p_partition);

// The per-texel minimum of the masks' distance values (inside either is inside, design D2): the one R8 field the
// shader samples. All non-empty masks must share one size; fails when none is non-empty.
Result<L8Image> combat_area_min_field(std::span<const L8Image> p_masks);

// ---- census (gate G3; bf3dump minimap --census) -------------------------------------------------------------------
// Over a library (ctest ui.minimap: "@all-mp"): every partition under levels/<l>/.../ui/ whose primary is a
// VeniceUICombatAreaAsset is read with read_combat_area and each distance field it references is decoded with
// load_l8_texture; every UIDistanceFieldAsset there resolves to a texture that must decode as L8. Textures are read
// from the mount of the superbundle the TextureAsset partition came from (EbxLibrary::origin).
struct MinimapCensus {
	size_t partitions_scanned = 0; // names under levels/*/ui/ parsed
	size_t combat_area_assets = 0; // VeniceUICombatAreaAsset partitions
	size_t combat_area_read = 0; // of them, read_combat_area succeeded
	size_t levels_with_areas = 0;
	size_t null_surroundings = 0;
	size_t null_distance_fields = 0; // a null DistanceField (Metro's unused Rush stage 5: a5, d5)
	size_t referenced_fields = 0; // distinct distance-field textures the assets reference
	size_t referenced_decoded = 0;
	size_t distance_field_assets = 0; // UIDistanceFieldAsset partitions
	size_t distance_field_decoded = 0;
	std::vector<std::string> failures; // "<name>: <error>", every one
};
Result<MinimapCensus> minimap_census(const EbxLibrary &p_library);

// ---- cache (design §8) ------------------------------------------------------------------------------------------------
// Nothing here is cached: Metro's load_level + traverse take 0.34 s + 0.04 s (bf3dump level, warm OS cache), the
// minimap reads after them resolve a handful of instances, and the textures are decoded at every load. The loader runs
// after the session's LoadStage::Done, so it adds nothing to the M1 load gates.

} // namespace bf3::core::ui
