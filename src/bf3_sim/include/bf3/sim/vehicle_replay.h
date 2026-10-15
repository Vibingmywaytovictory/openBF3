#pragma once
// The S2 replay harness and its judges (docs/design/s2.md §1, §7, §8, §10). Godot-free; bf3dump vehicle-replay and
// the sim.vehicle / sim.vehicle.unit tests call it.
//
// One course run (run_vehicle_course):
//   1. server = VehicleWorld over the shapes; add the vehicle in slot 0 at the course start; settle kSettleTicks with
//      kSettleCommand (full brake, nothing else: the vehicle is held on any slope and the wake rule of
//      vehicle_world.h keeps it awake, so tick 0, the end of the settle, is never asleep).
//   2. record: for ticks 1..ticks the scripted driver (vehicle_course.h) drives the server; keep the command log, the
//      full snapshot ring (VehicleWorld::save, every tick), the VehicleState and both hashes per tick.
//   3. G1 free driving: prediction = a second VehicleWorld over the same shapes, same slot; restore the server's
//      tick-0 snapshot; step the logged commands; at every tick compare state_hash() and vehicle_state_hash with the
//      server's. The first mismatch is reported with vehicle_state_first_difference, else per Jolt section: both
//      worlds are saved again section by section (VehicleWorld::save_section: global, bodies, contacts,
//      constraints; Jolt's combined stream has no section markers) and the first differing section and byte are
//      named.
//   4. G1 replay: for each restore tick t (fixed list + seeded draws), restore the server's tick-t snapshot into the
//      prediction world and replay min(replay_length, ticks - t) ticks, comparing as in 3; the same with the server
//      world itself rewound (in-place rollback).
//   5. G2 corrections: a client loop in the prediction world: at N = 0, snapshot_interval, 2 x snapshot_interval, ...
//      with N + R <= ticks (1,798 samples for R = 6 and 1,796 for R = 9 over 3,600 ticks), take the server's state
//      at N, quantise it (vehicle_ghost.h), decode it into a copy of the client's own state(slot) at N, restore per
//      mode (below), replay R ticks of logged commands, and measure |origin_client(N+R) - origin_server(N+R)|
//      (VehicleBodyState::origin, the frame origin). The replayed states become the client's history. Samples are
//      tagged from the server's recorded listener counts (never the client world's): "wheel contact" (>= 1 wheel in
//      contact at N+R) and "static collision" (chassis-static contacts in any tick of N..N+R).
// Restore modes (s2.md §7.3); G2 is judged on OwnHistory, the others are the ablations that say which state matters:
//   OwnHistory    client's own full snapshot at N, then the ghost (vehicle_restore::kGhost), contacts kept; the
//                 sleep test stays the client's own
//   Invalidate    the same, then the chassis contact cache invalidated
//   GhostLambdas  OwnHistory + the server's exact lambdas (what sending lambdas would buy)
//   ExactPod      client's own snapshot at N + the server's VehicleState with vehicle_restore::kAll, unquantised:
//                 the vehicle's bytes then equal the server's exactly, so the only difference left is the client's
//                 own contact cache (plan R4's "full-precision own-state block for the owner")
//   PodOwnLambdas ExactPod with the lambdas left the client's own (kAll & ~kLambdas): the unquantised POD minus
//                 the solver state the ghost does not carry, so PodOwnLambdas - ExactPod is the cost of not sending
//                 lambdas with no quantisation in the way
//   Full          the server's full snapshot at N (no ghost; must give 0 error, a harness self-check)
// ExactPod vs Full isolates the contact cache; OwnHistory vs ExactPod isolates quantisation and the unsent groups;
// GhostLambdas and PodOwnLambdas split the lambdas from the quantisation from both sides.
//
// Gate D (s2.md §1, §8.6): a course counts for G1 / G2 only if the server run actually drove it
// (vehicle_course_drove: distance, top speed, awake ticks, turns, impacts per kind). A parked or stalled Humvee is
// trivially deterministic, so G1 and G2 fail when any found course fails D instead of passing on it.
//
// G1m (run_vehicle_group): several vehicles in one world (slots 0..n-1), each with its own scripted course, that
// touch each other and cross tile seams; G1 and G1r as above over all slots, no G2. One vehicle cannot show the
// state Jolt does not save (broadphase layout, mNextLayerToUpdate, the order of tied wheel-cast hits); several can.

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/vehicle_course.h"
#include "bf3/sim/vehicle_ghost.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::sim {

inline constexpr uint32_t kSettleTicks = 120;
// The settle command: full brake, no throttle, no steer (s2.md §8.4). Non-zero, so the wake rule (vehicle_world.h)
// keeps the chassis active through the settle and every course starts awake at tick 0.
inline constexpr VehicleCommand kSettleCommand{ 0, 0, 0, 255, 0, 0, 0 };
// Gate thresholds (s2.md §1).
inline constexpr uint32_t kG1Ticks = 3600; // 60 s at 60 Hz
inline constexpr float kG2MaxError = 0.02f; // m, max and p99 per course
inline constexpr uint32_t kG2SnapshotInterval = 2; // 30 Hz snapshots (plan D4)
inline constexpr std::array<uint32_t, 2> kG2ReplayTicks{ 6, 9 }; // 100 ms and 150 ms

enum class VehicleRestoreMode : uint8_t { OwnHistory = 0, Invalidate = 1, GhostLambdas = 2, ExactPod = 3, PodOwnLambdas = 4, Full = 5 };
inline constexpr size_t kVehicleRestoreModeCount = 6;
// "own", "invalidate", "lambdas", "pod", "pod-own-lambdas", "full"
const char *vehicle_restore_mode_name(VehicleRestoreMode p_mode);

struct VehicleReplayOptions {
	uint32_t ticks = kG1Ticks;
	// G1 replay: restore ticks (clipped to ticks - 1) + seeded draws, and the replay length.
	std::vector<uint32_t> restore_ticks{ 1, 7, 600, 1234, 1800, 2999, 3500 };
	uint32_t random_restores = 8;
	uint32_t replay_length = 600;
	uint64_t seed = 0x5332u;
	// G2
	uint32_t snapshot_interval = kG2SnapshotInterval;
	std::vector<uint32_t> replay_ticks{ kG2ReplayTicks.begin(), kG2ReplayTicks.end() };
	uint32_t modes = 0x3Fu; // bit per VehicleRestoreMode; the primary mode is always run (sim.vehicle under asan: 0x01)
	VehicleGhostQuantisation quantisation = kDefaultVehicleGhost;
	bool g1 = true, g2 = true;
	bool keep_hashes = true; // fill VehicleCourseReport::server_hashes and log (bf3dump --hash-out / --log-out, the cross-binary check)
};

struct VehicleDivergence {
	Tick tick = 0;
	// vehicle_state_first_difference, or "jolt:<section>+<offset>" (section global / bodies / contacts / constraints,
	// from VehicleWorld::save_section of both worlds) when only the Jolt stream differs
	std::string field;
};

struct VehicleErrorStats {
	size_t samples = 0;
	float max = 0; // m
	float p99 = 0; // nearest rank
	float mean = 0;
	Tick worst_tick = 0; // N of the worst sample
};

struct VehicleCorrectionReport {
	VehicleRestoreMode mode = VehicleRestoreMode::OwnHistory;
	uint32_t replay_ticks = 0;
	VehicleErrorStats all, wheel_contact, static_collision;
	float max_orientation_degrees = 0; // reported (2 acos|q.q'| via a series, no libm trig)
	float max_velocity_error = 0; // m/s, reported
};

struct VehicleCourseReport {
	VehicleCourse course;
	uint32_t ticks = 0; // the run's length (VehicleReplayOptions::ticks); G1 needs g1_ticks == ticks
	// G1
	uint32_t g1_ticks = 0;
	uint32_t g1_mismatches = 0;
	VehicleDivergence g1_first;
	uint32_t replay_runs = 0;
	uint32_t replay_ticks = 0;
	uint32_t replay_mismatches = 0;
	VehicleDivergence replay_first;
	// G2
	std::vector<VehicleCorrectionReport> corrections; // per (mode, replay length)
	// course facts of the server run, ticks 1..ticks (gate D reads them, vehicle_course_drove; all printed)
	float distance = 0; // m driven (sum of |com step| per tick)
	float max_speed_kmh = 0;
	uint32_t ticks_all_wheels_off = 0; // airborne ticks (the jump)
	uint32_t ticks_any_wheel_off = 0; // ticks with at least one wheel out of contact (bumps unload wheels)
	uint32_t ticks_static_contact = 0;
	uint32_t static_impacts = 0; // rising edges of "chassis touches a static body" at >= 10 km/h
	float max_impact_speed_kmh = 0;
	uint32_t ticks_asleep = 0; // server body inactive; expected > 0 on SlopeIdle only (reported, not a gate)
	uint32_t yaw_rate_sign_changes = 0; // sign changes of the yaw rate while above 15 km/h (Slalom)
	float yaw_turns = 0; // |accumulated heading change| / 2 pi (Circle)
	float creep = 0; // m, SlopeIdle: com displacement from tick 600 to the end (held on the slope)
	bool drove = false; // gate D for this course (vehicle_course_drove)
	std::string drove_why; // the first failed criterion
	size_t snapshot_bytes = 0; // one full snapshot
	uint64_t steps = 0; // VehicleWorld::step calls over the whole run, every world and mode (printed with seconds)
	double seconds = 0; // wall time of the whole run (noisy on a shared machine)
	// The server's state_hash() at ticks 0..ticks (keep_hashes). bf3dump vehicle-replay --hash-out writes them, one
	// hex line per tick, so two binaries (dev vs release, later Windows vs Linux, M3) can be compared with a diff.
	std::vector<uint64_t> server_hashes;
	// The recorded commands of ticks 1..ticks (with keep_hashes): bf3dump vehicle-replay --log-out writes them
	// (format_vehicle_command_log, scratch only) and --log-in replays such a log open loop.
	VehicleCommandLog log;
};

core::Result<VehicleCourseReport> run_vehicle_course(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model,
		const VehicleCourse &p_course, const VehicleReplayOptions &p_options = {});

// Gate D for one course (s2.md §8.6): the server run drove the course, so G1 / G2 on it mean something. Every
// driving kind needs ticks_asleep == 0 (the scripts never send an all-zero command, so the wake rule of
// vehicle_world.h keeps the chassis awake). Per kind (kVehicleActivity, indexed by VehicleCourseKind):
//   Straight   distance >= 300 m, max speed >= 60 km/h
//   Brake      distance >= 300 m, max speed >= 55 km/h
//   Slalom     distance >= 300 m, max speed >= 30 km/h, yaw_rate_sign_changes >= 20
//   Circle     yaw_turns >= 3
//   Bumps      distance >= 300 m, max speed >= 45 km/h, ticks_any_wheel_off >= 30
//   Collision  static_impacts >= 3, ticks_static_contact >= 30
//   SlopeIdle  creep <= 0.05 m (held by the stand-still brake). Sleeping is reported only: Jolt's sleep test on a
//              braked slope is inferred behaviour, not a determinism requirement
// A course not found is not judged (gate C covers it). r_why gets the first failed criterion.
struct VehicleActivityThresholds {
	float min_distance = 0; // m
	float min_max_speed_kmh = 0;
	uint32_t min_yaw_rate_sign_changes = 0;
	float min_yaw_turns = 0;
	uint32_t min_ticks_any_wheel_off = 0;
	uint32_t min_static_impacts = 0;
	uint32_t min_ticks_static_contact = 0;
	float max_creep = 0; // m (0 = not checked)
	bool must_stay_awake = true; // ticks_asleep == 0
};
inline constexpr std::array<VehicleActivityThresholds, kVehicleCourseKindCount> kVehicleActivity{ {
		{ 300.0f, 60.0f, 0, 0.0f, 0, 0, 0, 0.0f, true }, // Straight
		{ 300.0f, 55.0f, 0, 0.0f, 0, 0, 0, 0.0f, true }, // Brake
		{ 300.0f, 30.0f, 20, 0.0f, 0, 0, 0, 0.0f, true }, // Slalom
		{ 0.0f, 0.0f, 0, 3.0f, 0, 0, 0, 0.0f, true }, // Circle
		{ 300.0f, 45.0f, 0, 0.0f, 30, 0, 0, 0.0f, true }, // Bumps
		{ 0.0f, 0.0f, 0, 0.0f, 0, 3, 30, 0.0f, true }, // Collision
		{ 0.0f, 0.0f, 0, 0.0f, 0, 0, 0, 0.05f, false }, // SlopeIdle
} };
bool vehicle_course_drove(const VehicleCourseReport &p_report, std::string *r_why = nullptr);

// The gates over a set of course reports (s2.md §1). C = every one of the kVehicleCourseKindCount kinds found (a
// course-finder result, kept apart so a missing wall never reads as "Jolt not deterministic"). D = every found
// course drove (vehicle_course_drove). G1 and G2 judge the found courses, need at least one, and fail when D fails
// for any found course, so a stalled course never passes them: G1 = g1_ticks == ticks, 0 mismatches, 0 replay
// mismatches; G2 = for OwnHistory at every replay length, all.samples > 0, all.max < kG2MaxError and
// all.p99 < kG2MaxError (Collision, when found, additionally needs static_collision.samples >= 30). p99 is
// informative once max passes. S2 passes only when C, D, G1 (with G1r), G1m, G2, I and U all pass.
bool vehicle_gate_courses(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why = nullptr);
bool vehicle_gate_drove(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why = nullptr);
bool vehicle_gate_g1(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why = nullptr);
bool vehicle_gate_g2(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why = nullptr);

// ---- G1m: several vehicles in one world ------------------------------------------------------------------------

struct VehicleGroupReport {
	uint32_t vehicles = 0;
	uint32_t ticks = 0; // the run's length (VehicleReplayOptions::ticks)
	uint32_t g1_ticks = 0;
	uint32_t g1_mismatches = 0;
	VehicleDivergence g1_first; // field prefixed "slot<i>." for a VehicleState difference
	uint32_t replay_runs = 0;
	uint32_t replay_ticks = 0;
	uint32_t replay_mismatches = 0;
	VehicleDivergence replay_first;
	uint32_t ticks_vehicle_contact = 0; // ticks with a chassis-chassis manifold (the case must have some)
	uint32_t slot0_ticks_asleep = 0; // the idling slot (reported: whether the sleep / wake-by-contact path ran)
	std::vector<float> slot_distance; // m per slot (slots >= 1 must drive >= 100 m; slot 0 is only pushed)
	uint64_t steps = 0;
	double seconds = 0;
};

// p_courses[i] drives slot i (2 <= size <= kMaxVehicleSlots). G1 and G1r as in run_vehicle_course (options.g2 is
// ignored). The gate: g1_ticks == ticks, 0 mismatches, 0 replay mismatches, ticks_vehicle_contact >= 30 and
// slot_distance[i] >= 100 m for every slot i >= 1 (the group moved: gate D's "never pass parked" rule).
core::Result<VehicleGroupReport> run_vehicle_group(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model,
		std::span<const VehicleCourse> p_courses, const VehicleReplayOptions &p_options = {});
bool vehicle_gate_g1m(const VehicleGroupReport &p_report, std::string *r_why = nullptr);

// Human-readable report blocks (the tests and bf3dump vehicle-replay print the same text): the course, the activity
// facts and gate D, G1 / G1r, G2 per mode and replay length (max / p99 / mean per subset), steps and seconds.
std::string format_vehicle_course_report(const VehicleCourseReport &p_report);
std::string format_vehicle_group_report(const VehicleGroupReport &p_report);

// ---- handling numbers (reported, unverified placeholders for S5 Session 2) ------------------------------------

struct VehicleHandling {
	float time_0_60_kmh = 0; // s, full throttle from rest on flat ground (0 = not reached in 30 s)
	float speed_at_6s_kmh = 0; // IW4L's feel target: 68 km/h at 6 s (plan §7.4) [report claim]
	float brake_distance_60_m = 0; // m, full brake from 60 km/h (reached under full throttle) to rest
	float brake_deceleration = 0; // m/s^2, mean over that stop (IW4L ~10 m/s^2)
	float turning_circle_m = 0; // m, kerb to kerb: diameter of the outer front wheel's contact path at full lock, 15 km/h
	float turning_circle_com_m = 0; // m, the centre of mass path's diameter
	float top_speed_kmh = 0; // after 60 s full throttle
};

// On build_synthetic_vehicle_ground (no features), the model's own numbers.
core::Result<VehicleHandling> measure_vehicle_handling(std::shared_ptr<const VehicleModel> p_model);

// ---- spawn settle (report row S; plan §3.4's wheeled first check "every ground spawn settles within 1 cm") --------

inline constexpr float kSettleDriftLimit = 0.01f; // m; reported against, not an S2 gate (M4 makes it one)
inline constexpr uint32_t kSettleDriftTicks = 60; // the last 60 of the kSettleTicks

struct VehicleSettleReport {
	Vec3 spawn{}; // the spawner position the vehicle was placed at
	float drop = 0; // m, spawn height above the settled origin
	float drift_y = 0; // m, max |y(t) - y(kSettleTicks)| of the com over the last kSettleDriftTicks settle ticks
	float drift = 0; // m, the same for the full 3D com position
	uint8_t wheels_in_contact = 0; // at the end of the settle
	bool settled = false; // drift < kSettleDriftLimit and every wheel in contact
};

// Each placement settled alone in a fresh VehicleWorld over p_shapes with kSettleCommand for kSettleTicks.
core::Result<std::vector<VehicleSettleReport>> measure_vehicle_settle(std::shared_ptr<const StaticWorldShapes> p_shapes,
		std::shared_ptr<const VehicleModel> p_model, std::span<const VehiclePlacement> p_placements);

} // namespace bf3::sim
