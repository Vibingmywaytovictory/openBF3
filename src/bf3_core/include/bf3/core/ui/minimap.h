#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/math.h"
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
}

namespace bf3::core::level {
class LoadedLevel;
class World;
}

namespace bf3::core::ui {

inline constexpr uint32_t kMaxMinimapImageSide = 4096;
inline constexpr size_t kMaxCombatAreas = 64;
inline constexpr size_t kMaxObjectives = 64;
inline constexpr float kResearchCombatAreaThreshold = 0.496f;

struct MinimapFieldParams {
	std::string distance_field_asset;
	std::string texture;
	float alpha_threshold = 0.5f;
	float distance_scale = 1.0f;
	float outline_inner = 0.0f;
	float outline_outer = 0.0f;
	std::array<float, 4> color_tint{ 1, 1, 1, 1 };
	std::array<float, 4> outline_color{ 0, 0, 0, 0 };
};

struct MinimapData {
	std::string level_id;
	std::string partition;
	std::array<float, 2> world_center{};
	float world_size = 0.0f;
	float world_rotation = 0.0f;
	float world_range = 0.0f;
	float air_radar_range = 0.0f;
	std::string satellite;
	std::array<float, 4> detail_texture_tint{ 1, 1, 1, 1 };
	MinimapFieldParams detail;
	MinimapFieldParams vegetation;
	float combat_area_alpha_threshold = kResearchCombatAreaThreshold;
	float combat_area_distance_scale = 0.0f;
	float combat_area_scale = 1.0f;
	float combat_area_fade_speed = 1.0f;
	std::array<float, 4> combat_area_color{ 1, 1, 1, 1 };
	std::string combat_area_multiply_texture;
	std::array<float, 2> combat_area_multiply_wrap{ 1, 1 };
	std::string fade_texture;
	std::string air_radar_fade_texture;
	bool use_combat_area_texture = true;
	bool use_streaming_minimap = false;
};

struct MinimapMapping {
	float center_x = 0.0f;
	float center_z = 0.0f;
	float size = 1.0f;
};

inline MinimapMapping minimap_mapping(const MinimapData &p_data) {
	return { p_data.world_center[0], p_data.world_center[1], p_data.world_size };
}

struct MinimapUv {
	float u = 0.0f;
	float v = 0.0f;
};

constexpr MinimapUv world_to_minimap_uv(const MinimapMapping &p_m, float p_x, float p_z) {
	return { 0.5f - (p_x - p_m.center_x) / p_m.size, 0.5f - (p_z - p_m.center_z) / p_m.size };
}

constexpr std::array<float, 2> minimap_uv_to_world(const MinimapMapping &p_m, MinimapUv p_uv) {
	return { p_m.center_x + (0.5f - p_uv.u) * p_m.size, p_m.center_z + (0.5f - p_uv.v) * p_m.size };
}

inline constexpr uint8_t kMinimapMappingVariants = 8;
constexpr MinimapUv world_to_minimap_uv_variant(const MinimapMapping &p_m, float p_x, float p_z, uint8_t p_variant) {
	const float a = (p_x - p_m.center_x) / p_m.size;
	const float b = (p_z - p_m.center_z) / p_m.size;
	const float u = (p_variant & 1) ? 0.5f + a : 0.5f - a;
	const float v = (p_variant & 2) ? 0.5f + b : 0.5f - b;
	return (p_variant & 4) ? MinimapUv{ v, u } : MinimapUv{ u, v };
}

struct MinimapView {
	float center_x = 0.0f;
	float center_y = 0.0f;
	float pixels_per_metre = 1.0f;
	float player_x = 0.0f;
	float player_z = 0.0f;
	float heading = 0.0f;
	bool rotate = true;
};

struct ScreenPoint {
	float x = 0.0f;
	float y = 0.0f;
};

inline ScreenPoint minimap_project(const MinimapView &p_view, float p_x, float p_z) {

	const float sx = -(p_x - p_view.player_x) * p_view.pixels_per_metre;
	const float sy = -(p_z - p_view.player_z) * p_view.pixels_per_metre;
	if (!p_view.rotate) {
		return { p_view.center_x + sx, p_view.center_y + sy };
	}

	const float c = std::cos(p_view.heading);
	const float s = std::sin(p_view.heading);
	return { p_view.center_x + sx * c - sy * s, p_view.center_y + sx * s + sy * c };
}

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

inline float minimap_marker_angle(const MinimapView &p_view) {
	return p_view.rotate ? 0.0f : -p_view.heading;
}

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

inline MinimapUv minimap_expected_uv(const MinimapView &p_view, const MinimapMapping &p_mapping, ScreenPoint p_screen, uint8_t p_variant = 0) {
	const std::array<float, 2> w = minimap_unproject(p_view, p_screen);
	return world_to_minimap_uv_variant(p_mapping, w[0], w[1], p_variant);
}
inline constexpr float kMinimapUvCheckTexels = 1.0f;

inline constexpr float kMinimapUvMirrorTexels = 10.0f;

inline float minimap_uv_error_texels(MinimapUv p_a, MinimapUv p_b, uint32_t p_side) {
	const float du = std::fabs(p_a.u - p_b.u) * float(p_side);
	const float dv = std::fabs(p_a.v - p_b.v) * float(p_side);
	return du > dv ? du : dv;
}

struct CombatAreaMask {
	std::string asset;
	std::string prefix;
	std::string distance_field_asset;
	std::string texture;
	std::string surrounding_asset;
	std::string surrounding_texture;
	std::string entity;
};

enum class ObjectiveKind : uint8_t {
	Flag,
	Hq,
};

struct MinimapObjective {
	ObjectiveKind kind = ObjectiveKind::Flag;
	core::Vec3 position{};
	float yaw = 0.0f;
	int32_t initial_owner = 0;
	std::string label_sid;
	std::string location_sid;
	std::string entity;
};

inline constexpr std::string_view kMinimapTextureOpacityOption = "profile/minimaptextureopacity";
inline constexpr std::string_view kMinimapZoomSmallOption = "profile/minimapzoomwhenmapissmall";
inline constexpr std::string_view kMinimapZoomLargeOption = "profile/minimapzoomwhenmapislarge";
inline constexpr std::string_view kMinimapInitGraph = "ui/flow/graph/ingame/playingminimapinit";
inline constexpr std::string_view kMinimapZoomNode = "MapZoom";
inline constexpr size_t kMaxMinimapZoomSteps = 16;

struct MinimapProfile {
	float texture_opacity = 0.0f;
	int32_t zoom_small_step = 0;
	int32_t zoom_large_step = 0;
	std::vector<float> zoom_steps;
	size_t zoom_nodes = 0;

	float zoom_small() const { return zoom_small_step >= 0 && size_t(zoom_small_step) < zoom_steps.size() ? zoom_steps[size_t(zoom_small_step)] : 0.0f; }
};

Result<MinimapProfile> read_minimap_profile(const EbxResolver &p_db, level::WorkBudget &r_budget);

struct MinimapLevel {
	MinimapData data;
	MinimapProfile profile;
	std::string mode;
	std::vector<CombatAreaMask> combat_areas;
	std::vector<MinimapObjective> objectives;
	size_t work = 0;
};

Result<MinimapData> read_minimap_data(const EbxResolver &p_db, const EbxInstance &p_level_root, std::string_view p_level_id,
		level::WorkBudget &r_budget);

Result<CombatAreaMask> read_combat_area(const EbxResolver &p_db, std::string_view p_asset_partition, level::WorkBudget &r_budget);

Result<MinimapLevel> read_minimap_level(const level::LoadedLevel &p_level, const level::World &p_world);

struct MinimapLoadOptions {

	unsigned threads = 0;
};
inline constexpr unsigned kMinimapHudLoaderThreads = 2;

Result<MinimapLevel> load_minimap_level(std::shared_ptr<const Install> p_install, std::string_view p_level_id, std::string_view p_mode,
		const MinimapLoadOptions &p_options = {});

struct L8Image {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> texels;

	bool empty() const { return texels.empty(); }

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

constexpr bool sdf_inside(uint8_t p_texel, float p_threshold) {
	return float(p_texel) / 255.0f < p_threshold;
}

struct MinimapImages {
	L8Image satellite;
	L8Image detail;
	L8Image vegetation;
	std::vector<L8Image> combat_areas;
	std::vector<L8Image> surroundings;
};

Result<L8Image> load_l8_texture(const LevelMount &p_mount, std::string_view p_res_name);

Result<L8Image> decode_l8_texture(const res::DxTexture &p_texture, std::span<const uint8_t> p_chunk, std::string_view p_name);

Result<LevelMount> mount_minimap_level(const Install &p_install, std::string_view p_level_id);
Result<MinimapImages> load_minimap_images(const LevelMount &p_mount, const MinimapLevel &p_level);

std::vector<L8Image> l8_mip_chain(const L8Image &p_image);

Result<std::vector<bool>> combat_area_union(std::span<const L8Image> p_masks, float p_threshold);

double mask_box_iou(const std::vector<bool> &p_mask, uint32_t p_side, const MinimapMapping &p_mapping, uint8_t p_variant,
		std::array<float, 4> p_box_x0_x1_z0_z1);

double mask_polygon_iou(const std::vector<bool> &p_mask, uint32_t p_side, const MinimapMapping &p_mapping, uint8_t p_variant,
		std::span<const std::array<float, 2>> p_polygon);

inline constexpr uint32_t kMaxIouSide = 1024;
inline constexpr size_t kMaxAreaPolygonPoints = 1024;

Result<std::vector<std::array<float, 2>>> read_area_polygon(const level::LoadedLevel &p_level, const level::World &p_world,
		std::string_view p_partition);

Result<L8Image> combat_area_min_field(std::span<const L8Image> p_masks);

struct MinimapCensus {
	size_t partitions_scanned = 0;
	size_t combat_area_assets = 0;
	size_t combat_area_read = 0;
	size_t levels_with_areas = 0;
	size_t null_surroundings = 0;
	size_t null_distance_fields = 0;
	size_t referenced_fields = 0;
	size_t referenced_decoded = 0;
	size_t distance_field_assets = 0;
	size_t distance_field_decoded = 0;
	std::vector<std::string> failures;
};
Result<MinimapCensus> minimap_census(const EbxLibrary &p_library);

}
