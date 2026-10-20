#include <doctest/doctest.h>

#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/host/game_server.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/bot_fill_stats.h"
#include "bf3/host/level_load.h"
#include "bf3/host/scale_bench.h"
#include "bf3/sim/bots/bot_stats.h"
#include "bf3/sim/bots/walker_brain.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/static_world.h"

#include <cmath>
#include <cstdint>
#include <type_traits>

using namespace bf3;

static_assert(sizeof(sim::NavPolyRef) == 8);

static_assert(sim::kNavTileSize == 64.0f);

static_assert(std::is_trivially_copyable_v<host::BotFillStats>);
static_assert(std::is_trivially_copyable_v<sim::NavQueryStats>);
static_assert(std::is_trivially_copyable_v<sim::bots::WalkerStats>);
static_assert(std::is_trivially_copyable_v<sim::bots::PathServiceStats>);
static_assert(std::is_trivially_copyable_v<host::TailShares>);
static_assert(std::is_trivially_copyable_v<sim::NavPointCheck>);
static_assert(std::is_trivially_copyable_v<sim::bots::BotGoal>);
static_assert(std::is_trivially_copyable_v<host::GameServerTickTimes>);

static_assert((sim::nav_flags::kWalk & sim::nav_flags::kCrouch) == 0 && (sim::nav_flags::kCrouch & sim::nav_flags::kDrop) == 0);
static_assert(sim::kNavAreaWalk != sim::kNavAreaCrouch && sim::kNavAreaCrouch != sim::kNavAreaDrop && sim::kNavAreaWalk != 0);
static_assert(sim::kMaxNavCorners >= 2 && sim::kMaxNavPathPolys >= 64);

TEST_CASE("s1 contract: nav defaults are the plan's and the soldier's") {
	const sim::NavParams p;

	CHECK(p.cell_size == 0.2f);
	CHECK(p.radius_round_up);
	CHECK(std::ceil(p.agent_radius / p.cell_size) * p.cell_size >= p.agent_radius);
	CHECK(p.cell_height == 0.1f);

	const float climb_cells = p.agent_climb / p.cell_height;
	CHECK(std::fabs(climb_cells - std::round(climb_cells)) < 1e-4f);

	CHECK(std::lround(climb_cells) == 3);

	const sim::MotorParams m;
	CHECK(p.agent_radius == m.radius);
	CHECK(p.max_slope_degrees == m.max_ascend_degrees);
	CHECK(p.crouch_height < p.agent_height);

	CHECK(std::sqrt(2.0f * 9.81f * p.drop_max) < 13.9f);
	CHECK(p == sim::NavParams{});

	const float recast_border = (std::ceil(p.agent_radius / p.cell_size) + 3.0f) * p.cell_size;
	CHECK(recast_border < p.drop_reach + p.agent_radius);

	sim::NavStats st;
	st.seconds_input = 1.0;
	st.seconds_bake = 2.0;
	st.seconds_assemble = 0.5;
	st.seconds_link_setup = 0.25;
	CHECK(sim::nav_bake_gate_seconds(st) == 3.5);
	const sim::NavBakeOptions bo;
	CHECK(bo.link_shapes == nullptr);
}

TEST_CASE("s1 contract: walker and bench defaults") {
	const sim::bots::WalkerParams w;
	CHECK(w.think_hz >= 5.0f);
	CHECK(w.think_hz <= 10.0f);
	CHECK(w.objective_share >= 0.0f);
	CHECK(w.objective_share <= 1.0f);
	const host::ScaleBenchOptions b;
	CHECK(b.walkers == 64);
	CHECK(b.seconds >= 120.0);
	CHECK(b.observers >= 1);
	CHECK(host::kScaleGateP99Ms == 8.0);
	CHECK(host::kValidityMetres == 100.0);
	CHECK(host::kValidityWalkedShare == 0.9);
	CHECK(host::kValidityAbandonedShare == 0.1);
	CHECK(host::kBotBudgetP99Ms == 2.0);
	CHECK(double(host::kServerMemoryBudgetBytes) == doctest::Approx(0.4 * 1024.0 * 1024.0 * 1024.0).epsilon(1e-6));
	CHECK(b.paced_seconds > 0.0);

	const host::BotFillConfig fc;
	CHECK(fc.spawn_in_main);
	const host::BotSpawns bs;
	CHECK(bs.points.empty());
	CHECK(!bs.teams_merged);
	const host::BotPointRow row;
	CHECK(!row.in_main_weak);
	const sim::NavPointCheck pc;
	CHECK(pc.weak_component == UINT32_MAX);
}

TEST_CASE("s1 contract: region tile helpers (mode_areas)") {
	using Tile = std::array<int32_t, 2>;

	const std::vector<Tile> four = core::level::point_tiles({ 10.0f, 0.0f, 10.0f }, 32.0f);
	CHECK(four.size() == 4);
	CHECK(core::level::point_tiles({ 10.0f, 0.0f, 10.0f }, 0.0f) == std::vector<Tile>{ Tile{ 0, 0 } });
	CHECK(core::level::point_tiles({ NAN, 0.0f, 0.0f }, 32.0f).empty());

	core::level::CombatAreaDef a;
	a.polygon = { { 10.0f, 10.0f }, { 20.0f, 10.0f }, { 20.0f, 20.0f }, { 10.0f, 20.0f } };
	const auto small = core::level::combat_area_tiles(std::span<const core::level::CombatAreaDef>(&a, 1), 0.0f);
	REQUIRE(small);
	CHECK(*small == std::vector<Tile>{ Tile{ 0, 0 } });
	core::level::CombatAreaDef huge;
	huge.polygon = { { -9e5f, -9e5f }, { 9e5f, -9e5f }, { 9e5f, 9e5f }, { -9e5f, 9e5f } };
	CHECK(!core::level::combat_area_tiles(std::span<const core::level::CombatAreaDef>(&huge, 1), 32.0f));
}

TEST_CASE("s1 contract: the additive M1 fields default to M1's behaviour") {

	const sim::NavBakeOptions nb;
	CHECK(nb.water.empty());
	const host::GameServerConfig gc;
	CHECK(gc.bots == nullptr);
	const host::GameServerStats gs;
	CHECK(gs.bots.bots == 0);
	CHECK(gs.motor_steps == 0);
	CHECK(core::level::kMaxCombatAreaPoints >= 64);
}
