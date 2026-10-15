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
// layer, on a fixed grid in a fixed order (deterministic), and keeps every leg, and the turnaround area beyond each
// end of the Straight / Brake / Slalom legs (those 60 s runs turn around repeatedly), on tiles that have terrain
// (MP_007's soup has terrain only in tiles with Havok triangles or their neighbours; s2.md §4.4). Each kind:
//   Straight   a flat straight of >= 150 m: full throttle from rest, brake to rest at the end, turn around, again
//   Brake      the same straight: accelerate to 60 km/h, full brake, measure, repeat
//   Slalom     +-full steer at 0.5 Hz around the straight's line at ~40 km/h, turning back at its ends
//   Circle     full lock left at a held ~15 km/h on a flat disc of >= 25 m radius (turning circle)
//   Bumps      a line crossing ground whose height profile has a curvature peak (kerb, berm, crest) at >= 50 km/h;
//              a crest steep enough to unload all four wheels makes it the jump case
//   Collision  ~30 km/h square into a static Collision-layer wall (a horizontal ray hit with |normal.y| < 0.3 and
//              >= 1.5 m tall), reverse 15 m, again
//   SlopeIdle  stop on ground of 8..20 degrees, no input for the rest of the run (stand-still brake, sleep)
// A kind not found in the search radius is reported as not found: gate C fails, and G1 / G2 judge the found courses
// only (s2.md §1), so a finder miss is never read as a determinism result.

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
};

struct VehicleCourseSearch {
	float radius = 300.0f; // m around each seed
	float grid = 4.0f; // m between candidate points
	float max_flat_degrees = 4.0f; // "flat" for Straight / Brake / Slalom / Circle
	size_t max_queries = 2000000; // CollisionWorld queries over the whole search (hostile / huge levels stop here)
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
	uint8_t leg = 0; // which end of the leg it drives towards
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
