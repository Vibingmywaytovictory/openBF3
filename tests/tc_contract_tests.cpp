#include <doctest/doctest.h>

#include "bf3/core/level/collision_soup.h"
#include "bf3/core/res/terrain.h"
#include "bf3/render/collision_prep.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim_harness/nav_input_check.h"

#include <cstdint>
#include <string_view>
#include <type_traits>

using namespace bf3;

static_assert(sim::kStaticTerrainBodyLayer == 3);
static_assert(sim::kStaticTerrainBodyLayer >= sim::kStaticLayerCount);
static_assert(sim::kStaticTerrainBodyLayer < 4);

static_assert(sim::kStaticTerrainBits >= 1 && sim::kStaticTerrainBits <= 16);
static_assert(sim::kStaticTerrainBlock >= 2 && sim::kStaticTerrainBlock <= 8);

static_assert(sim::kMaxStaticTerrains == core::level::kMaxSoupTerrains);

static_assert(2 * (core::res::kMaxGridSide - 1) * (core::res::kMaxGridSide - 1) >= core::level::kMaxSoupTriangles);

static_assert(sim::kNoStaticTile == UINT32_MAX);
static_assert(sim::kNoStaticTerrain == UINT32_MAX);
static_assert(std::is_aggregate_v<sim::StaticTerrainView>);
static_assert(std::is_trivially_copyable_v<sim::StaticTerrainInfo>);

static_assert(std::is_same_v<decltype(&core::level::hash_soup_terrain),
		core::Result<core::level::SoupTerrainHash> (*)(const core::level::SoupTerrain &, uint32_t, core::level::WorkBudget &, unsigned)>);
static_assert(std::is_same_v<decltype(&sim::static_terrain_content), core::Sha256Digest (*)(const sim::StaticTerrainView &)>);

TEST_CASE("tc contract: the terrain tag is the soup's terrain cell tag") {
	constexpr sim::SurfaceTag tag = sim::static_terrain_tag();
	CHECK(tag.material == 0);
	CHECK(tag.surface == core::level::surface::kDirt);
	CHECK(tag.body == uint8_t(core::level::CollisionBody::Collision));
	CHECK(tag.flags == core::level::kSoupTerrain);
	CHECK(tag.reserved == 0);
}

TEST_CASE("tc contract: the additive fields default to the behaviour before TC") {
	const sim::StaticHit hit;
	CHECK(hit.tile == 0);
	CHECK(hit.terrain == sim::kNoStaticTerrain);
	const core::level::SoupTerrain terrain;
	CHECK(terrain.grid == nullptr);
	CHECK(terrain.frame == core::res::Transform::identity());
	const core::level::SoupTerrainCell cell;
	CHECK(cell.count == 0);
	CHECK_FALSE(cell.open);
	const core::level::SoupTerrainHash hash;
	CHECK(hash.hash == core::level::kFnvOffset);
	CHECK(hash.tiles.empty());
	const sim::NavTileInput input;
	CHECK(input.unwalkable.empty());
	const sim::NavBakeOptions bake;
	CHECK(bake.water.empty());
	const sim::NavInputCheck check;
	CHECK(check.terrain_matched == 0);
	const sim::NavStats nav_stats;
	CHECK(nav_stats.tiles_wet == 0);
	CHECK(nav_stats.triangles_unwalkable == 0);
	const sim::StaticWorldStats stats;
	CHECK(stats.terrains == 0);
	CHECK(stats.terrain_bytes == 0);
	const sim::StaticTerrainView view;
	CHECK(view.axes[0][0] == 1.0f);
	CHECK(view.axes[1][1] == 1.0f);
	CHECK(view.axes[2][2] == 1.0f);
	const render::CollisionPrepOptions prep;
	CHECK(prep.terrain);
	CHECK(prep.terrain_radius > 0.0f);
	CHECK(prep.max_terrain_triangles > 0);

	CHECK(core::level::kCollisionSoupVersion == 2);
	CHECK(input.terrain_triangles == 0);
	CHECK(check.terrain_tolerance == 0.0f);
	CHECK(std::string_view(sim::kStaticTerrainCacheKind) == "jolt-terrain");
}
