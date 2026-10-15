// S1 interface contract (docs/design/s1.md §10, §11): every header of spike S1 compiles together with the M1 headers
// it extends, and the plain types and constants keep the properties the design relies on. No data, no Godot; nothing
// here calls a function the S1 implementation has yet to define.
#include <doctest/doctest.h>

#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_load.h"
#include "bf3/host/scale_bench.h"
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

// Poly refs are 64-bit (RECASTNAVIGATION_DT_POLYREF64, docs §5.3); nav.cpp static_asserts dtPolyRef against it.
static_assert(sizeof(sim::NavPolyRef) == 8);
// The nav tile is the soup's and the static world's 64 m tile (one tile key for all three).
static_assert(sim::kNavTileSize == 64.0f);
// Stats travel through GameServerStats copies across threads (ServerHost::game_stats).
static_assert(std::is_trivially_copyable_v<host::BotFillStats>);
static_assert(std::is_trivially_copyable_v<sim::NavQueryStats>);
static_assert(std::is_trivially_copyable_v<sim::bots::WalkerStats>);
static_assert(std::is_trivially_copyable_v<sim::NavPointCheck>);
static_assert(std::is_trivially_copyable_v<sim::bots::BotGoal>);
// Flags and areas are distinct bits / ids.
static_assert((sim::nav_flags::kWalk & sim::nav_flags::kCrouch) == 0 && (sim::nav_flags::kCrouch & sim::nav_flags::kDrop) == 0);
static_assert(sim::kNavAreaWalk != sim::kNavAreaCrouch && sim::kNavAreaCrouch != sim::kNavAreaDrop && sim::kNavAreaWalk != 0);
static_assert(sim::kMaxNavCorners >= 2 && sim::kMaxNavPathPolys >= 64);

TEST_CASE("s1 contract: nav defaults are the plan's and the soldier's") {
	const sim::NavParams p;
	CHECK(p.cell_size == 0.25f); // plan §3.6
	CHECK(p.cell_height == 0.1f);
	// climb / ch must be whole (topics/06 §2.4): 0.3 / 0.1 = 3 cells.
	const float climb_cells = p.agent_climb / p.cell_height;
	CHECK(std::fabs(climb_cells - std::round(climb_cells)) < 1e-4f);
	// The defaults equal the motor's default soldier (MotorParams defaults = Metro's mpsoldier, sim.motor checks).
	const sim::MotorParams m;
	CHECK(p.agent_radius == m.radius);
	CHECK(p.max_slope_degrees == m.max_ascend_degrees);
	CHECK(p.crouch_height < p.agent_height);
	// A drop link never deals fall damage: v = sqrt(2 g h) stays below the 13.9 m/s knee (plan §7.2) at g 9.81.
	CHECK(std::sqrt(2.0f * 9.81f * p.drop_max) < 13.9f);
	CHECK(p == sim::NavParams{});
}

TEST_CASE("s1 contract: walker and bench defaults") {
	const sim::bots::WalkerParams w;
	CHECK(w.think_hz >= 5.0f); // plan §3.6: brains at 5-10 Hz, staggered
	CHECK(w.think_hz <= 10.0f);
	CHECK(w.objective_share >= 0.0f);
	CHECK(w.objective_share <= 1.0f);
	const host::ScaleBenchOptions b;
	CHECK(b.walkers == 64);
	CHECK(b.seconds >= 120.0); // plan: at least 2 minutes of sim time
	CHECK(b.observers >= 1);
	CHECK(host::kScaleGateP99Ms == 8.0);
}

TEST_CASE("s1 contract: the additive M1 fields default to M1's behaviour") {
	const core::level::SoupOptions so;
	CHECK(so.terrain_cover.empty());
	const sim::StaticWorldLoadOptions wo;
	CHECK(wo.terrain_cover.empty());
	const host::SharedLoadOptions lo;
	CHECK(lo.terrain_cover.empty());
	const host::GameServerConfig gc;
	CHECK(gc.bots == nullptr);
	const host::GameServerStats gs;
	CHECK(gs.bots.bots == 0);
	CHECK(gs.motor_steps == 0);
	CHECK(core::level::kMaxCombatAreaPoints >= 64);
}
