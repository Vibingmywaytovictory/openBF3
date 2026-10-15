#pragma once
// S2 driving courses and command logs (docs/design/s2.md §8). Godot-free.
//
// A course is a start pose plus a closed-loop scripted driver. The driver runs on the server side only: it reads the
// server world's VehicleState each tick and emits the VehicleCommand the server steps; the commands are recorded into
// a VehicleCommandLog, and every other world (prediction, replay) is fed that log open loop, never the driver. So the
// driver may use any arithmetic (it is not part of the determinism contract), but its output is quantised commands.
//
// Courses on a real level are found, not hand-placed: find_vehicle_courses searches around seed points (the level's
// vehicle spawners, vehicle_spawns.h) with CollisionWorld queries (ground_below, ray, overlap_box) on the Collision
// layer, on a fixed grid in a fixed order (deterministic), and keeps every leg, and the area beyond each end of
// the Straight / Brake / Slalom legs (a 20 m turnaround disc; for Bumps the line itself extended 16 m: the overshoot
// room of the shuttle below; Bumps also checks the cells beside its line, at most 8 degrees), on tiles that have terrain
// (MP_007's soup has terrain only in tiles with Havok triangles or their neighbours; s2.md §4.4). The four leg
// kinds shuttle: forward a -> b, stop a few metres past b, reverse back to past a at 30 km/h, stop, again (a U-turn
// at full lock would swing ~15 m sideways onto unchecked ground); a leg that climbs runs downhill forward. Each kind:
//   Straight   a flat straight of >= 150 m: full throttle forward, brake to rest past the end, reverse back, again
//   Brake      the same straight: accelerate to 60 km/h, full brake, measure, on to the end, reverse back, repeat
//   Slalom     +-full steer at 0.5 Hz around the straight's line, ~40 km/h forward and 30 km/h in reverse
//   Circle     full lock left at a held ~15 km/h on a flat disc of >= 25 m radius (turning circle)
//   Bumps      a line crossing ground whose height profile has a curvature peak (kerb, berm, crest) at 55 km/h
//              forward (>= 50 km/h), 30 km/h in reverse; a crest steep enough to unload all four wheels makes it the
//              jump case
//   Collision  ~30 km/h square into a static Collision-layer wall (a horizontal ray hit with |normal.y| < 0.3 and
//              >= 1.5 m tall), push against it for 0.75 s, reverse 15 m, again
//   SlopeIdle  on ground of 8..20 degrees: full brake command for 240 ticks (the stand-still timer reaches its 2 s
//              limit while the brake holds the vehicle), then the all-zero command for the rest of the run: the
//              stand-still brake holds it, and with no input the wake rule stops and Jolt's sleep test may put it
//              to sleep (reported, not a gate)
// Every other script never sends the all-zero command (a stop is a held brake), so with the wake rule
// (vehicle_world.h) a driving course never sleeps; gate D (vehicle_replay.h vehicle_course_drove) checks it.
// A kind not found in the search radius is reported as not found: gate C fails, and G1 / G2 judge the found courses
// only (s2.md §1), so a finder miss is never read as a determinism result. Each kind is searched with its own
// query budget (VehicleCourseSearch::max_queries_per_kind) and reports what it spent, so a miss says whether the
// terrain or the budget ran out.

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/vehicle_types.h"
#include "bf3/sim/vehicle_world.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::sim {

class CollisionWorld;

enum class VehicleCourseKind : uint8_t { Straight = 0, Brake = 1, Slalom = 2, Circle = 3, Bumps = 4, Collision = 5, SlopeIdle = 6 };
inline constexpr size_t kVehicleCourseKindCount = 7;
const char *vehicle_course_name(VehicleCourseKind p_kind); // "straight", "brake", "slalom", "circle", "bumps", "collision", "slope_idle"

struct VehicleCourse {
	VehicleCourseKind kind = VehicleCourseKind::Straight;
	bool found = false;
	VehiclePlacement start; // on the ground (settled by the harness before tick 0)
	Vec3 a{}, b{}; // the leg: straight ends, slalom line, bump line; circle centre in a; wall point in b
	float slope_degrees = 0; // SlopeIdle: the ground's slope at start
	float feature = 0; // Bumps: the curvature peak (1/m); Collision: wall height (m)
	uint32_t seed = 0; // the seed point it was found from (index into p_seeds)
	size_t queries = 0; // CollisionWorld queries this kind's search spent (found or not)
	bool budget_exhausted = false; // the search stopped at max_queries_per_kind (a miss for budget, not terrain)
};

struct VehicleCourseSearch {
	// m around each seed. 600 (s2.md §15): at 300 m every 150 m flat leg on MP_007 failed its 20 m turnaround discs
	// (489 legs: 134 without ground, 160 too steep, 195 blocked); at 600 m all kinds are found within their budgets.
	float radius = 600.0f;
	float grid = 4.0f; // m between candidate points
	float max_flat_degrees = 4.0f; // "flat" for Straight / Brake / Slalom / Circle
	// CollisionWorld queries per kind (hostile / huge levels stop here); the whole search is bounded by
	// kVehicleCourseKindCount x this. Kinds are searched independently in kind order, so a late kind is never
	// starved by an early one.
	size_t max_queries_per_kind = 400000;
	// Straight / Brake / Slalom: a disc of this radius beyond each leg end must be on terrain tiles, within
	// max_turnaround_degrees and free of Collision-layer obstacles up to the chassis height.
	float turnaround_radius = 20.0f;
	float max_turnaround_degrees = 10.0f;
};

// One course per kind (found or not), kind order. Deterministic for equal inputs.
core::Result<std::array<VehicleCourse, kVehicleCourseKindCount>> find_vehicle_courses(const CollisionWorld &p_world,
		std::span<const Vec3> p_seeds, const VehicleModel &p_model, const VehicleCourseSearch &p_search = {});
// The courses on build_synthetic_vehicle_ground's features (vehicle_world.h), all found.
std::array<VehicleCourse, kVehicleCourseKindCount> synthetic_vehicle_courses(const SyntheticVehicleGround &p_ground = {});
// G1m (vehicle_replay.h run_vehicle_group): three courses for slots 0..2 on the synthetic ground that make the
// vehicles touch and cross a 64 m tile seam: slot 0 idles (SlopeIdle on the flat), slot 1 drives a Collision course
// whose wall point b is slot 0's side (~30 km/h, reverse, again), slot 2 slaloms along a line crossing the seam and
// passing within a metre of both.
std::array<VehicleCourse, 3> synthetic_vehicle_group(const SyntheticVehicleGround &p_ground = {});

// ---- the scripted driver (server side) -----------------------------------------------------------------------

struct VehicleScriptMemory {
	uint8_t phase = 0;
	uint8_t leg = 0; // reserved (the leg shuttle runs a -> b forward and b -> a in reverse)
	uint32_t phase_ticks = 0;
	float measured[4] = {}; // phase measurements (start speed, start position along the leg, ...)
};

// The command for tick p_tick given the server's state at the end of the previous tick.
VehicleCommand drive_vehicle_course(const VehicleCourse &p_course, const VehicleModel &p_model, const VehicleState &p_state, Tick p_tick,
		VehicleScriptMemory &r_memory);

// ---- command logs --------------------------------------------------------------------------------------------

struct VehicleCommandLog {
	std::vector<VehicleCommand> commands; // consecutive ticks from commands[0].tick
};

inline constexpr size_t kMaxVehicleLogCommands = 1u << 20; // ~4.9 h at 60 Hz

// "OBF3VLOG 1" header, then one line per command: "tick throttle brake steer buttons" (decimal). '#' comments.
std::string format_vehicle_command_log(const VehicleCommandLog &p_log);
// Refuses: a missing or other header, a line over 128 bytes, a field out of range (throttle / steer -127..127,
// brake 0..255, buttons within vehicle_buttons::kWireMask), a tick that is not the previous + 1, more than
// p_max_commands commands. The error names the line.
core::Result<VehicleCommandLog> parse_vehicle_command_log(std::string_view p_text, size_t p_max_commands = kMaxVehicleLogCommands);

} // namespace bf3::sim
