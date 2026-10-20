#pragma once

#include "bf3/core/install.h"
#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/water_plane.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/havok.h"
#include "bf3/core/result.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::res {
struct HeightGrid;
}

namespace bf3::core::level {

namespace surface {
inline constexpr uint8_t kBark = 1;
inline constexpr uint8_t kBrick = 2;
inline constexpr uint8_t kCarpet = 3;
inline constexpr uint8_t kCloth = 4;
inline constexpr uint8_t kConcrete = 5;
inline constexpr uint8_t kDirt = 6;
inline constexpr uint8_t kFlesh = 7;
inline constexpr uint8_t kFoliage = 8;
inline constexpr uint8_t kGlass = 9;
inline constexpr uint8_t kGrass = 10;
inline constexpr uint8_t kGravel = 11;
inline constexpr uint8_t kIce = 12;
inline constexpr uint8_t kMetal = 13;
inline constexpr uint8_t kMud = 14;
inline constexpr uint8_t kPaper = 15;
inline constexpr uint8_t kPlaster = 16;
inline constexpr uint8_t kRock = 17;
inline constexpr uint8_t kSand = 18;
inline constexpr uint8_t kSnow = 19;
inline constexpr uint8_t kWater = 20;
inline constexpr uint8_t kWood = 21;
inline constexpr uint8_t kAsphalt = 22;
inline constexpr uint8_t kCeramic = 23;
inline constexpr uint8_t kPlastic = 24;
inline constexpr uint8_t kRubber = 25;
inline constexpr uint8_t kCushion = 26;
inline constexpr uint8_t kFruit = 27;
inline constexpr uint8_t kPaintedMetal = 28;
inline constexpr uint8_t kSlush = 30;
}
inline constexpr size_t kSurfaceCount = 32;

std::string_view surface_name(uint8_t p_surface);

std::optional<uint8_t> classify_surface(std::string_view p_name);

inline constexpr uint32_t kMaterialSeeThrough = 0x0800'0000;
inline constexpr uint32_t kMaterialPairMask = 0x00FF'FFFF;

class SurfaceMaterials {
public:
	enum class Lookup : uint8_t {
		Surface,
		Unnamed,
		Unknown,
	};

	static Result<SurfaceMaterials> read(const EbxResolver &p_db, const EbxInstance &p_root, WorkBudget &r_budget);

	size_t pair_count() const { return pairs_.size(); }
	size_t resolved_pairs() const;
	size_t row_count() const { return rows_.size(); }
	const std::vector<std::optional<uint8_t>> &row_surfaces() const { return rows_; }

	Lookup lookup(uint32_t p_value, uint8_t &r_surface) const;

private:
	std::vector<std::optional<uint8_t>> pairs_;
	std::vector<uint32_t> index_map_;
	std::vector<std::optional<uint8_t>> rows_;
};

enum class CollisionBody : uint8_t { Collision = 0, Raycast = 1, Character = 2 };
inline constexpr size_t kCollisionBodyCount = 3;
std::string_view collision_body_name(uint8_t p_body);
inline constexpr uint8_t kSoupSeeThrough = 1;
inline constexpr uint8_t kSoupInvisible = 2;
inline constexpr uint8_t kSoupTerrain = 4;

struct SoupTriangle {
	std::array<res::Vec3, 3> v{};
	uint32_t material = 0;
	uint32_t placement = 0;
	uint8_t surface = 0;
	uint8_t body = 0;
	uint8_t flags = 0;
};

struct SoupPlacement {
	enum class State : uint8_t {
		Built,
		Hidden,
		Missing,
		NoRes,
	};
	CollisionPlacement::Kind kind = CollisionPlacement::Kind::Group;
	uint32_t index = 0;
	std::string res;
	State state = State::Built;
	std::string error;
	size_t triangles = 0;
	size_t destroyed = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	uint64_t hash = 0;
};

struct SoupTerrain {
	enum class State : uint8_t {
		Built,
		Missing,
		NoRes,
	};
	uint32_t object = 0;
	std::string asset;
	State state = State::Built;
	std::string error;
	uint8_t depth = 0;
	size_t side = 0;
	size_t holes = 0;
	size_t cells = 0;
	size_t open = 0;

	size_t triangles = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	uint64_t hash = 0;

	std::shared_ptr<const res::HeightGrid> grid;
	res::Transform frame{};
};

struct SoupTile {
	int32_t x = 0;
	int32_t z = 0;
	std::vector<SoupTriangle> triangles;
	uint64_t sum = 0;
	res::Vec3 min{};
	res::Vec3 max{};
};

struct SoupProgress {
	enum Stage : int { Mount = 0, Traverse, Placements, Materials, Decode, Triangles, Terrain, Tiles, Done, kStages };
	std::atomic<int> stage{ Mount };
	std::atomic<size_t> done{ 0 };
	std::atomic<size_t> total{ 0 };
};
std::string_view soup_stage_name(int p_stage);

struct SoupOptions {
	float tile_size = 64.0f;
	unsigned threads = 0;
	bool terrain = true;
	float terrain_spacing = 2.0f;
	SoupProgress *progress = nullptr;

};

inline constexpr uint32_t kCollisionSoupVersion = 2;

inline constexpr size_t kMaxSoupTriangles = size_t(1) << 25;

inline constexpr size_t kMaxSoupTerrains = 8;

struct SoupTerrainCell {
	std::array<SoupTriangle, 2> triangles{};
	uint8_t count = 0;
	uint8_t degenerate = 0;
	uint8_t nonfinite = 0;
	bool open = false;
};
SoupTerrainCell soup_terrain_cell(const SoupTerrain &p_terrain, uint32_t p_placement, size_t p_x, size_t p_z);

Result<size_t> append_soup_terrain_tile(const SoupTerrain &p_terrain, uint32_t p_placement, const std::array<int32_t, 2> &p_tile,
		std::vector<SoupTriangle> &r_out, WorkBudget &r_budget);

struct SoupTerrainTileSum {
	std::array<int32_t, 2> tile{};
	size_t triangles = 0;
	uint64_t sum = 0;
};
struct SoupTerrainHash {
	size_t cells = 0;
	size_t open = 0;
	size_t triangles = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	uint64_t hash = 0xcbf29ce484222325ull;
	std::vector<SoupTerrainTileSum> tiles;
};
Result<SoupTerrainHash> hash_soup_terrain(const SoupTerrain &p_terrain, uint32_t p_placement, WorkBudget &r_budget, unsigned p_threads = 0);

struct SoupSource {
	enum class Kind : uint8_t { Ebx = 0, Res = 1, Chunk = 2 };
	Kind kind = Kind::Ebx;
	std::string name;
	Guid id;
	auto operator<=>(const SoupSource &) const = default;
};

struct CollisionSoup {
	std::vector<SoupPlacement> placements;
	std::vector<SoupTerrain> terrains;
	std::vector<SoupTile> tiles;
	size_t triangles = 0;
	size_t terrain_triangles = 0;
	size_t destroyed = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	size_t built = 0;
	size_t hidden = 0;
	size_t missing = 0;
	size_t no_res = 0;
	uint64_t sum = 0;
	std::array<size_t, kSurfaceCount> surfaces{};
	std::array<size_t, kCollisionBodyCount> bodies{};
	res::Vec3 min{};
	res::Vec3 max{};
	size_t res_decoded = 0;
	size_t material_pairs = 0;
	size_t resolved_pairs = 0;
	double seconds_placements = 0.0;
	double seconds_decode = 0.0;
	double seconds_triangles = 0.0;
	double seconds_terrain = 0.0;
	double seconds_tiles = 0.0;
	std::vector<SoupSource> sources;
};

Result<CollisionSoup> build_collision_soup(const LoadedLevel &p_level, const World &p_world, const SoupOptions &p_options = {});

bool soup_quantise(const std::array<res::Vec3, 3> &p_v, std::array<int64_t, 9> &r_q);
bool soup_degenerate(const std::array<int64_t, 9> &p_q);
void soup_hash_bytes(uint64_t &r_hash, const std::array<int64_t, 9> &p_q, uint32_t p_material, uint8_t p_surface, uint8_t p_body, uint8_t p_flags);
inline constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull;
std::array<int32_t, 2> soup_tile_of(const std::array<int64_t, 9> &p_q);

struct SoupShapeResult {
	std::vector<SoupTriangle> triangles;
	std::vector<std::array<int32_t, 2>> tiles;
	size_t destroyed = 0;
	size_t degenerate = 0;
	size_t nonfinite = 0;
	uint64_t hash = 0;
	uint64_t sum = 0;
};
SoupShapeResult soup_shape_triangles(const res::HavokPhysicsData &p_havok, const std::vector<res::Shape> &p_roots, const res::Transform &p_frame,
		const std::vector<uint8_t> &p_bodies, const SurfaceMaterials &p_materials, std::string_view p_owner_name);

struct LevelCollision {
	std::unique_ptr<LoadedLevel> level;
	std::unique_ptr<World> world;
	CollisionSoup soup;
	std::vector<LevelTransform> spawns;
	double seconds_load = 0.0;
	double seconds_traverse = 0.0;
	double seconds_soup = 0.0;
};
Result<LevelCollision> load_level_collision(std::shared_ptr<const Install> p_install, std::string_view p_level_id, std::string_view p_mode,
		const SoupOptions &p_options = {});

}
