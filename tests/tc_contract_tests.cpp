// Terrain collision interface contract (track "Terrain collision everywhere", docs/design/terrain-collision.md §13):
// the TC additions to the soup, static world, collision world, nav and overlay headers compile together, and their
// constants and defaults keep the properties the design relies on. No data, no Godot, and no call to a function the
// TC implementation has yet to define (static_terrain_tag is inline).
#include <doctest/doctest.h>

#include "bf3/core/level/collision_soup.h"
#include "bf3/core/res/terrain.h"
#include "bf3/render/collision_prep.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/static_world.h"

#include <cstdint>
#include <string_view>
#include <type_traits>

using namespace bf3;

// Body user data: tile bodies use tile * 4 + layer with layer < kStaticLayerCount; the terrain bodies take the free
// value 3, so describe_hit tells them apart from the low two bits alone.
static_assert(sim::kStaticTerrainBodyLayer == 3);
static_assert(sim::kStaticTerrainBodyLayer >= sim::kStaticLayerCount);
static_assert(sim::kStaticTerrainBodyLayer < 4);
// Jolt 5.6 HeightFieldShapeSettings limits (mBitsPerSample 1..16, mBlockSize 2..8).
static_assert(sim::kStaticTerrainBits >= 1 && sim::kStaticTerrainBits <= 16);
static_assert(sim::kStaticTerrainBlock >= 2 && sim::kStaticTerrainBlock <= 8);
// The static world refuses exactly what the soup refuses.
static_assert(sim::kMaxStaticTerrains == core::level::kMaxSoupTerrains);
// A whole 4097 x 4097 terrain as triangles would reach the soup's triangle cap: why the terrain is a record.
static_assert(2 * (core::res::kMaxGridSide - 1) * (core::res::kMaxGridSide - 1) >= core::level::kMaxSoupTriangles);
// The sentinels are not valid indices of any world the caps allow.
static_assert(sim::kNoStaticTile == UINT32_MAX);
static_assert(sim::kNoStaticTerrain == UINT32_MAX);
static_assert(std::is_aggregate_v<sim::StaticTerrainView>);
static_assert(std::is_trivially_copyable_v<sim::StaticTerrainInfo>);

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
	// Until the TC implementation lands the soup version stays M1's (the implementation bumps it to 2).
	CHECK(core::level::kCollisionSoupVersion == 1);
	CHECK(std::string_view(sim::kStaticTerrainCacheKind) == "jolt-terrain");
}
