#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/static_world.h"

#include <array>
#include <functional>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Cache;
}
namespace bf3::core::level {
struct CollisionSoup;
class LoadedLevel;
class World;
}

namespace bf3::sim {

struct MotorParams;

using NavPolyRef = uint64_t;

inline constexpr float kNavTileSize = 64.0f;

inline constexpr uint8_t kNavAreaWalk = 1;
inline constexpr uint8_t kNavAreaCrouch = 2;
inline constexpr uint8_t kNavAreaDrop = 3;
namespace nav_flags {
inline constexpr uint16_t kWalk = 1u << 0;
inline constexpr uint16_t kCrouch = 1u << 1;
inline constexpr uint16_t kDrop = 1u << 2;
inline constexpr uint16_t kAll = kWalk | kCrouch | kDrop;
}

enum class NavPartition : uint8_t { Watershed = 0, Monotone = 1, Layers = 2 };

struct NavParams {

	float cell_size = 0.2f;
	float cell_height = 0.1f;
	float agent_radius = 0.35f;
	float agent_height = 1.8f;
	float crouch_height = 1.15f;
	float agent_climb = 0.3f;
	float max_slope_degrees = 50.0f;

	bool radius_round_up = true;
	float max_edge_length = 12.0f;
	float max_simplification_error = 1.3f;
	uint32_t min_region_cells = 8 * 8;
	uint32_t merge_region_cells = 20 * 20;
	float detail_sample_distance = 6.0f;

	float detail_sample_max_error = 2.0f;
	NavPartition partition = NavPartition::Watershed;

	bool drop_links = true;
	float drop_max = 4.9f;
	float drop_reach = 1.5f;
	float drop_sample_spacing = 1.0f;
	float region_margin = 32.0f;

	bool operator==(const NavParams &) const = default;
};

NavParams nav_params_from_motor(const MotorParams &p_motor);

std::string nav_params_text(const NavParams &p_params);

std::string recast_build_id();
inline constexpr uint32_t kNavFormatVersion = 1;
inline constexpr uint32_t kNavConverterVersion = 1;
inline constexpr const char *kNavTileCacheKind = "nav-tile";
inline constexpr const char *kNavWorldCacheKind = "nav-world";
inline constexpr const char *kNavAreaCacheKind = "nav-area";

inline constexpr size_t kMaxNavTileInputTriangles = size_t(1) << 21;
inline constexpr uint64_t kMaxNavTileRasterCells = uint64_t(1) << 28;
inline constexpr uint64_t kMaxNavInputRefs = uint64_t(1) << 26;
inline constexpr float kMaxNavTileHeight = 800.0f;
inline constexpr size_t kMaxNavTiles = size_t(1) << 16;

using NavTileKey = std::array<int32_t, 2>;

float nav_input_border(const NavParams &p_params);

int nav_recast_border_cells(const NavParams &p_params);

struct NavTileInput {
	NavTileKey tile{};
	std::vector<float> vertices;
	std::vector<uint8_t> layers;
	size_t own_triangles = 0;
	size_t far_triangles = 0;

	core::Sha256Digest digest{};

	std::vector<uint8_t> unwalkable;

	size_t terrain_triangles = 0;
};

bool nav_terrain_wet(const std::array<Vec3, 3> &p_triangle, std::span<const core::level::WaterPlane> p_water);

core::Result<std::vector<NavTileInput>> nav_tile_inputs(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles,
		const NavParams &p_params, uint32_t p_layer_mask = query_mask::kMovement);

core::Result<std::vector<NavTileInput>> nav_tile_inputs(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles,
		const NavParams &p_params, uint32_t p_layer_mask, std::span<const core::level::WaterPlane> p_water);

struct NavTileInfo {
	NavTileKey tile{};
	uint32_t input_triangles = 0;
	uint32_t polys = 0;
	uint32_t vertices = 0;
	uint32_t detail_triangles = 0;
	uint32_t drop_links = 0;
	uint32_t data_bytes = 0;
	core::Sha256Digest input{};
	double seconds = 0.0;
};

struct NavStats {
	size_t tiles = 0;
	size_t tiles_empty = 0;
	size_t polys = 0;
	size_t drop_links = 0;
	uint64_t data_bytes = 0;
	size_t tiles_hit = 0;
	size_t tiles_baked = 0;
	bool world_hit = false;
	size_t tiles_uncached = 0;
	std::string cache_error;
	size_t far_triangles = 0;
	size_t link_worlds = 0;
	size_t link_candidates = 0;
	size_t link_sweeps = 0;
	size_t links_rejected = 0;
	double seconds_input = 0.0;
	double seconds_bake = 0.0;
	double seconds_link_setup = 0.0;
	double seconds_link_cpu = 0.0;
	double seconds_assemble = 0.0;
	double seconds_restore = 0.0;
	double seconds_total = 0.0;
	uint64_t peak_working_set = 0;

	size_t tiles_wet = 0;
	size_t triangles_unwalkable = 0;
};

inline double nav_bake_gate_seconds(const NavStats &p_stats) {
	return p_stats.seconds_input + p_stats.seconds_bake + p_stats.seconds_assemble;
}

class NavWorld {
public:
	~NavWorld();
	NavWorld(const NavWorld &) = delete;
	NavWorld &operator=(const NavWorld &) = delete;

	const NavParams &params() const;
	size_t tile_count() const;
	const NavTileInfo &tile(size_t p_index) const;
	const NavStats &stats() const;
	size_t poly_count() const;
	Vec3 min() const;
	Vec3 max() const;

	struct Impl;
	const Impl &impl() const { return *impl_; }

private:
	friend struct NavWorldBuilder;
	explicit NavWorld(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

struct NavBakeOptions {
	NavParams params;
	unsigned threads = 0;
	uint32_t layer_mask = query_mask::kMovement;

	std::shared_ptr<const StaticWorldShapes> link_shapes;
	std::atomic<size_t> *tiles_done = nullptr;

	std::vector<core::level::WaterPlane> water;
};

core::Result<std::shared_ptr<const NavWorld>> bake_nav_world(const core::level::CollisionSoup &p_soup, std::span<const NavTileKey> p_tiles,
		const NavBakeOptions &p_options = {});

core::Result<std::shared_ptr<const NavWorld>> bake_nav_world(std::span<const StaticTriangle> p_triangles, std::span<const NavTileKey> p_tiles,
		const NavBakeOptions &p_options = {});

struct NavLoadOptions {
	NavBakeOptions bake;
	StaticWorldLoadOptions world;
	std::vector<NavTileKey> region;
};

core::Result<std::shared_ptr<const NavWorld>> load_nav_world_cached(core::Cache *p_cache, const core::level::LoadedLevel &p_level,
		const core::level::World &p_world, const core::level::CollisionSoup *p_soup, const StaticWorldShapes &p_shapes, const NavLoadOptions &p_options);

std::vector<uint8_t> encode_nav_tile_blob(const NavWorld &p_world, size_t p_tile);
core::Status validate_detour_tile(std::span<const uint8_t> p_data, const NavParams &p_params, const NavTileKey &p_tile);
core::Result<std::shared_ptr<const NavWorld>> decode_nav_tile_blobs(std::span<const std::span<const uint8_t>> p_tiles, const NavParams &p_params);

core::CacheKeyInputs nav_tile_key_inputs(const core::Sha256Digest &p_input, const NavParams &p_params, bool p_link_checked);
core::CacheKeyInputs nav_world_key_inputs(const core::Sha256Digest &p_source_digest, std::string_view p_level, std::string_view p_mode,
		const NavLoadOptions &p_options);

struct NavInputCheck {
	size_t nav_triangles = 0;
	size_t world_triangles = 0;
	size_t matched = 0;
	size_t nav_only = 0;
	size_t nav_only_degenerate = 0;
	size_t world_only = 0;
	float max_corner_error = 0.0f;

	size_t terrain_matched = 0;
	float terrain_tolerance = 0.0f;
};
core::Result<NavInputCheck> nav_check_input_against_world(const StaticWorldShapes &p_world, const NavTileInput &p_input, const NavParams &p_params,
		uint32_t p_layer_mask = query_mask::kMovement, float p_tolerance = 0.001f);

struct NavComponents {
	std::vector<uint32_t> component;
	std::vector<uint32_t> polys;
	std::vector<double> area;
	uint32_t largest = 0;
	size_t weak_count = 0;

	std::vector<uint32_t> weak_component;
};
NavComponents nav_components(const NavWorld &p_world);

std::vector<uint8_t> nav_components_reaching(const NavWorld &p_world, const NavComponents &p_components, uint32_t p_target);

std::vector<double> nav_component_area_in(const NavWorld &p_world, const NavComponents &p_components, const std::function<bool(float, float)> &p_inside);

uint32_t nav_poly_component(const NavWorld &p_world, const NavComponents &p_components, NavPolyRef p_poly);

struct NavPointCheck {
	NavPolyRef poly = 0;
	Vec3 nearest{};
	float horizontal = 0.0f;
	float vertical = 0.0f;
	uint32_t component = UINT32_MAX;
	uint32_t weak_component = UINT32_MAX;
};
NavPointCheck nav_check_point(const NavWorld &p_world, const NavComponents &p_components, const Vec3 &p_point, const Vec3 &p_half_extents);

struct NavDebugMesh {
	std::vector<Vec3> triangles;
	std::vector<uint32_t> triangle_component;
	std::vector<uint8_t> triangle_area;
	std::vector<Vec3> links;
};
NavDebugMesh nav_debug_mesh(const NavWorld &p_world, const NavComponents *p_components);

}
