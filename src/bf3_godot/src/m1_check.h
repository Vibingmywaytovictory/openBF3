#pragma once
// The M1 checks of Bf3LevelSession (docs/design/m1.md §1, §14). Main thread only; each runs as a frame-driven
// script after LoadStage::Done and drives the session the way a player would: through the input mapper (its toggles,
// clamp and spawn-view adoption), the client's presentation, the first-person camera and the dev commands.
//
// --m1-check (ctest godot.m1, headless; no scene, docs §11.1), with max_slots 128:
//   E1   through the full loopback path, measured on the server's per-tick Trace reports (DebugKind::Trace), never
//        on display-rate polling: at the first --m1-courses (6) spawn points whose in-process settle_spawn has a
//        find_clear_heading course (the same default arguments as sim.motor, so the same courses), each segment
//        starts with a Teleport to the settled feet facing the course: E1a walk / sprint / crouch / prone steady
//        speeds over ticks 60-120 of a full-forward command (the window's mean +-1 %, no tick above +1 %, per tick
//        >= 95 %: sim.motor's Metro reading, docs §1), E1b the jump apex 0.6 m +-1 mm, E1c the six transitions
//        18 / 30 / 42 / 42 / 63 / 18 ticks (S->C, C->P, P->S, S->P, P->C, C->S by the mapper's toggles; finished -
//        started + 1, docs §6.5); E1d drops of 9.8 / 10.1 / 46 m at find_drop_spot by Teleport (fall = h within
//        1 mm, impact within 1e-3 m/s of sqrt(2 g h), damage 0 / 20-21 / dead). Every snapshot's own state seen in
//        the frames during a trace equals the trace entry of its tick bit for bit.
//   E3   one SpawnSweep (60 ticks) through the live server: 102 rows, all ok, equal field for field to settle_spawn
//        rows computed in-process on the check's own CollisionWorld over the same shapes.
//   E2   for "150/2" (the gate) and "lat=150-165,loss=2" (a reported stress row; orchestrator decision on docs §16
//        question 2): a new ClientSession with the profile, SpawnAt the first course's point, 30 s idle, then 4
//        walk-stop cycles (2 s walk forward / back along the course heading, 13 s rest), one RestSample per presented
//        frame with the eye read back from the first-person camera, judged by host::judge_rest.
//   install.unchanged; a PASS/FAIL table and "M1 CHECK: PASS|FAIL (n/m)".
//   Headless also runs upload.surface_format: encode_rs_surface against the RenderingServer's own conversion, and
//   input.keys: the BF3 PC press keys through the device path with modifiers held.
//
// --m1-load-check=cold|warm (ctest godot.m1_load, windowed): E4 (cold, <= 15 s) or E5 (warm, <= 5 s) from process
//   start to the first controllable frame (orchestrator decision on docs §16 question 4; _ready -> Done is
//   reported), the cache warmth the run must have, per-phase work and wall times (Phase A stages, B from
//   SceneLaneStats, C1 task work, C2 main-thread work), the loading frames and their mean time, the upload counts
//   against the plan, the frame time p50 / p99 over 5 s after Done, the game's peak working set (reported; E6 gates
//   the converter, tools.m1_prepare), upload.surface_format on the real renderer, the shots, install.unchanged.

#include "bf3/host/rest_check.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3LevelSession;

// One PASS/FAIL/INFO row.
struct CheckRow {
	std::string check;
	int result = 2; // 0 pass, 1 fail, 2 info
	std::string detail;
};
// Prints the table and "<p_title>: PASS|FAIL (n/m)"; returns 0 when no row failed.
int print_check_table(const char *p_title, const std::vector<CheckRow> &p_rows);

// upload.surface_format: encode_rs_surface of synthetic surfaces (with and without tangents, both index widths)
// against mesh_add_surface_from_arrays + mesh_get_surface, byte for byte.
CheckRow check_surface_format();
// input.keys: key events (physical and logical keycode, pressed, not echo) through a fresh InputMapper's
// handle_event: Space / Left Ctrl / Z / F6 must jump / crouch / go prone / toggle the free camera with no modifier,
// Shift, Ctrl and Shift+Ctrl held (sprint-jump, sprint-crouch), and an echo or a release must do nothing.
CheckRow check_input_keys();

// Screenshots after Done (docs §11.8): free-camera views and first-person views at spawn points.
class ShotRunner {
public:
	struct FreeShot {
		std::string path;
		std::array<float, 5> view{}; // x, y, z, yaw, pitch (degrees, sim yaw)
	};
	struct SpawnShot {
		uint32_t index = 0;
		std::string mode = "stand"; // stand, crouch, prone, walk
	};
	// Parses --shots / --shot-spawns; false (with r_error) on a malformed entry.
	static bool parse(const std::string &p_shots, const std::string &p_spawns, std::vector<FreeShot> &r_free, std::vector<SpawnShot> &r_spawn,
			std::string &r_error);

	ShotRunner(Bf3LevelSession &p_session, std::vector<FreeShot> p_free, std::vector<SpawnShot> p_spawn, std::string p_dir);
	bool empty() const { return free_.empty() && spawn_.empty(); }
	// Every frame after Done; true when every shot was taken (or failed).
	bool frame(uint64_t p_now_ns);
	// L1 look: each free shot waits p_settle_s as well as 30 frames (auto exposure), then times p_measure frames
	// (the first only starts the clock, so 1 is raised to 2: one interval).
	void set_timing(double p_settle_s, uint32_t p_measure) {
		settle_s_ = p_settle_s;
		measure_frames_ = p_measure == 1 ? 2 : p_measure;
	}
	size_t saved() const { return saved_; }
	size_t failed() const { return failed_; }
	const std::vector<std::string> &lines() const { return lines_; }

private:
	Bf3LevelSession &session_;
	std::vector<FreeShot> free_;
	std::vector<SpawnShot> spawn_;
	std::string dir_;
	size_t next_ = 0; // spawn shots first, then free shots
	int state_ = 0;
	uint64_t state_ns_ = 0;
	uint32_t life_ = 0;
	uint64_t frames_ = 0;
	uint32_t request_id_ = 7000;
	size_t saved_ = 0;
	size_t failed_ = 0;
	std::vector<std::string> lines_;
	// L1 look: settle and frame timing of the free shots.
	double settle_s_ = 0.0;
	uint32_t measure_frames_ = 0;
	uint32_t measured_ = 0;
	uint64_t last_ns_ = 0;
	double view_cpu_ms_ = 0.0, view_gpu_ms_ = 0.0, total_cpu_ms_ = 0.0, total_gpu_ms_ = 0.0;
	uint64_t total_frames_ = 0;
	// Per-frame samples of the current view (at most --shot-measure, capped at 100,000) for its medians, which other
	// processes' spikes move far less than the means; the sums of the per-view medians over the measured views.
	std::vector<float> view_cpu_samples_, view_gpu_samples_;
	double median_cpu_sum_ms_ = 0.0, median_gpu_sum_ms_ = 0.0;
	size_t median_views_ = 0;
};

class M1Check {
public:
	M1Check(Bf3LevelSession &p_session, uint32_t p_courses);
	~M1Check();
	// Every frame after Done, after the camera update. True when finished (exit_code()).
	bool frame(uint64_t p_now_ns);
	int exit_code() const { return exit_code_; }

private:
	struct Course {
		uint32_t point = 0; // spawn point index
		bf3::sim::Vec3 feet{}; // settle_spawn's settled feet
		uint16_t heading = 0;
	};
	enum class Phase : uint8_t {
		Prepare,
		E1Teleport,
		E1Segment,
		E1Report,
		E1dTeleport,
		E1dReport,
		E1dRespawn,
		E3,
		E2Start,
		E2Spawn,
		E2Settle,
		E2Record,
		Finish,
		Done,
	};

	void prepare();
	void add(const std::string &p_check, int p_result, const std::string &p_detail);
	void fail_phase(const std::string &p_check, const std::string &p_why);
	bool teleport(const bf3::sim::Vec3 &p_feet, uint16_t p_yaw, uint64_t p_now_ns);
	bool at_teleport(const bf3::sim::Vec3 &p_feet) const;
	void send_trace(uint32_t p_ticks);
	std::optional<bf3::net::DebugReport> take_report(uint32_t p_id);
	void collect_snapshot_states();
	void compare_snapshots(const std::vector<bf3::sim::SoldierState> &p_trace);
	void analyse_segment(const std::vector<bf3::sim::SoldierState> &p_trace);
	void analyse_drop(const std::vector<bf3::sim::SoldierState> &p_trace);
	void finish_e1();
	void judge_e2();
	void finish_rows();
	bool timed_out(uint64_t p_now_ns, double p_seconds) const;

	Bf3LevelSession &session_;
	uint32_t want_courses_ = 6;
	std::vector<CheckRow> rows_;
	bool failed_ = false;
	int exit_code_ = 1;
	Phase phase_ = Phase::Prepare;
	uint64_t phase_ns_ = 0;

	// In-process world (the same shapes and motor params as the server).
	std::unique_ptr<bf3::sim::CollisionWorld> world_;
	bf3::sim::SoldierMotor motor_;
	std::vector<bf3::sim::SettleResult> headless_rows_;
	std::vector<Course> courses_;
	std::optional<bf3::sim::Vec3> drop_;
	double prepare_seconds_ = 0.0;

	// E1 script.
	size_t course_ = 0;
	size_t segment_ = 0; // 0 walk, 1 sprint, 2 crouch, 3 prone, 4 jump, 5 transitions
	int step_ = 0;
	bf3::sim::Tick step_tick_ = 0;
	bf3::sim::Tick walk_tick_ = 0;
	uint32_t trace_id_ = 100;
	uint32_t request_id_ = 5000;
	std::map<bf3::sim::Tick, bf3::sim::SoldierState> server_own_; // snapshot own states seen during a trace
	uint64_t snapshots_compared_ = 0, snapshots_equal_ = 0;
	// Per kind: courses measured and passed, with the worst numbers.
	struct SpeedResult {
		size_t measured = 0, passed = 0;
		float lo = 1e9f, hi = 0.0f, worst_mean_error = 0.0f;
	};
	std::array<SpeedResult, 4> speeds_{};
	size_t jumps_ = 0, jumps_ok_ = 0;
	float apex_lo_ = 1e9f, apex_hi_ = -1e9f;
	size_t chains_ = 0, chains_ok_ = 0;
	std::string chain_ticks_;
	std::vector<std::string> e1_failures_;
	size_t drop_index_ = 0;
	float drop_height_ = 0.0f;
	uint32_t drop_life_ = 0;

	// E2.
	size_t profile_ = 0;
	std::vector<bf3::host::RestSample> samples_;
	uint64_t record_start_ns_ = 0;
	uint32_t e2_life_ = 0;
};

class M1LoadCheck {
public:
	M1LoadCheck(Bf3LevelSession &p_session, bool p_warm, ShotRunner *p_shots);
	// Every frame after Done; true when finished (exit_code()).
	bool frame(uint64_t p_now_ns, double p_delta);
	int exit_code() const { return exit_code_; }

private:
	void report();

	Bf3LevelSession &session_;
	bool warm_ = false;
	ShotRunner *shots_ = nullptr;
	int phase_ = 0; // 0 frame times, 1 shots, 2 report
	uint64_t start_ns_ = 0;
	std::vector<float> frame_ms_;
	int exit_code_ = 1;
};

} // namespace bf3::godot_side
