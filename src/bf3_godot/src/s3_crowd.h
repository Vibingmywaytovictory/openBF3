#pragma once
// Bf3Crowd: the S3 crowd node (docs/design/s3.md). Scene res://scenes/s3_crowd.tscn = Node3D root > Session
// (Bf3LevelSession, Metro TDM) + Crowd (Bf3Crowd). The node only reads the session through its public accessors
// (done(), failed(), shared_level(), install(), save_screenshot(), finish()), so no M1 file changes.
//
// The gate command (s3.md §1; the ctest wrapper tests/godot_s3_crowd.cmake runs exactly this):
//   <godot> --path bf3-opensource res://scenes/s3_crowd.tscn --
//       --s3-check --walkers=0 --no-hud --bf3-cache=<fresh scratch root> --quit-after=600
// The session reads --walkers, --no-hud and --bf3-cache itself (level_session.cpp parse_args) and exposes none
// of them, so Bf3Crowd parses the same OS user args (godot_util.h user_arg_value / has_user_arg) and, under
// --s3-check, refuses to measure (G0 config row FAIL) unless --walkers=0 and --no-hud are present. It opens the
// same cache root with godot_cache.h acquire_cache on its loader thread (one shared Cache per root in the
// process); the session's default root (user://bf3cache) is used when --bf3-cache is absent.
//
// Once the session reports done() and the loader thread has run bf3::render::load_soldier_assets, the node
// (main thread):
//  * creates its own bf3::sim::CollisionWorld over shared_level()->static_world (owner: the main thread) and
//    validates the fixed stage (crowd_stage.h): floor under every cell, no overlap, camera point clear, and the
//    camera-to-soldier rays unoccluded on Collision + Raycast. A failure is G0 FAIL ("stage blocked") with the
//    first failing cell; the check measures nothing on an invalid stage;
//  * builds 64 soldiers (32 US / 32 RU) and its own Camera3D from stage_camera (vertical FOV 60, KEEP_HEIGHT),
//    made current;
//  * per frame (_process): G1 timers start -> CrowdEvaluator::wait_and_publish -> SkinUploader::upload (roots +
//    changed palettes) -> set_inputs (stage_input) -> kick -> timers stop. On a frozen frame (R2 and the bench's
//    frozen windows) step is not called at all. The pose job runs on a bf3::core::WorkerGroup of
//    clamp(hardware_concurrency - 4, 2, 6) lanes (0 reported cores count as 2 lanes) while the frame renders.
//
// Timers per frame (s3.md §1): step wall time (Time::get_ticks_usec, G1a), step thread CPU and whole-frame main
// thread CPU (QueryThreadCycleTime deltas, G1b), the _process delta (G2). Thread cycles are converted to ms with a
// rate calibrated at the check's start: __rdtsc against QueryPerformanceCounter over 250 ms, printed, plus a
// self-test: the best of 20 busy windows of 5 ms must read thread cycles = TSC cycles within +-5 % (a single 100 ms loop,
// revision 2's test, fails on a loaded machine whenever the thread is preempted; else G0 config FAIL). Under the rendering
// thread model Separate the RS work leaves the main thread and G1b cannot see it, so the check reads
// rendering/driver/threads/thread_model and requires Safe (the project default; G0 config row).
// Vsync: the session restores the project's vsync mode and max fps at Done, so each run sets
// window_set_vsync_mode and Engine::set_max_fps(0) itself and prints the effective window_get_vsync_mode.
//
// Shutdown order (shutdown(), called by the check before session->finish() and from NOTIFICATION_PREDELETE):
// wait for the pose job, clear the uploader (frees every RID and node it made), free the CollisionWorld, join the
// loader thread (it holds a reference to session->install()), release the cache. Only then may the session
// finish: finish() runs the session's own shutdown and quit, and must not race a crowd still using its install.
//
// User args (after "--"):
//   --s3-backend=<rs|skeleton3d|texture>   skin upload backend outside the check (default rs, s3.md §7.3)
//   --s3-lod=<on|off>           temporal LOD outside the check (default on)
//   --s3-check                  the gate run (s3.md §1, §12): window 2560x1440 borderless at the primary screen's
//                               origin; G0 validity rows; backend bench; runs R1-R4; G3 shots and metrics; prints a
//                               table and "S3 CHECK: PASS|FAIL"; exits 0 / 1, or 77 when the primary screen is not
//                               2560x1440 (the wrapper also skips on no window / device and on "no usable install")
//   --s3-seconds=<s>            length of each measured run (default and minimum for the gate: 30)
//   --s3-shot-dir=<dir>         G3 shots (default <repo>/b/shots/s3; refused inside the install). Named apart from
//                               the session's --shot-dir (its --shot-spawns shots), so the two never cross.
//   --walkers, --no-hud, --bf3-cache, --quit-after   the session's; read here too (above)

#include "skin_upload.h"

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/crowd_stage.h"
#include "bf3/core/worker_group.h"
#include "bf3/render/soldier_assets.h"

#include <godot_cpp/classes/node3d.hpp>

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Camera3D;
}

namespace bf3::core {
class Cache;
}

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::godot_side {

class Bf3LevelSession;

// One measured run or window class of the check (s3.md §1, §12). Percentiles over the frames after warm-up.
struct CrowdRunStats {
	std::string label; // e.g. "R1 rs lod=off vsync=off"
	std::string vsync_mode; // window_get_vsync_mode as read back during the run
	uint32_t frames = 0;
	double seconds = 0.0;
	double fps = 0.0;
	double step_p50_ms = 0.0, step_p99_ms = 0.0; // G1a: step wall time
	double step_cpu_p50_ms = 0.0; // step's own main-thread CPU (QueryThreadCycleTime)
	double other_cpu_p50_ms = 0.0; // main-thread CPU of the frame minus step's (the G1b input)
	double frame_p50_ms = 0.0, frame_p99_ms = 0.0; // G2: _process delta
	double render_cpu_p50_ms = 0.0, render_gpu_p50_ms = 0.0; // viewport measured render times
	double job_p50_ms = 0.0, job_p99_ms = 0.0; // worker job wall time
	double blocked_p99_ms = 0.0; // main thread waiting on the job
	double evaluated_mean = 0.0; // soldiers posed per frame
	std::array<double, bf3::anim::TemporalLodTable::kTiers + 2> evaluated_by_tier_per_s{}; // R3: per tier (CrowdFrameStats)
	uint32_t calls_p50 = 0; // Godot calls per frame by the uploader
	// G0 rows of this run.
	uint32_t in_view_min = 0; // minimum over sampled frames (every 30th) of soldiers inside the frustum
	double two_cycle_fraction = 0.0, idle_mix_fraction = 0.0, mean_loco_clips = 0.0; // run blend coverage
};

// G1b of one backend or of R2 (s3.md §1): animated vs frozen windows interleaved in one run.
struct HiddenCostStats {
	CrowdRunStats animated;
	CrowdRunStats frozen;
	double hidden_p50_ms = 0.0; // other_cpu_p50(animated) - other_cpu_p50(frozen); may be negative (noise)
};

class Bf3Crowd : public godot::Node3D {
	GDCLASS(Bf3Crowd, godot::Node3D)

public:
	Bf3Crowd();
	~Bf3Crowd() override;

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	// One measured run of the check (s3.md §12): its configuration and the per-frame samples.
	struct RunSpec {
		std::string label;
		SkinBackend backend = SkinBackend::RsSkeleton;
		bool lod = false;
		bool lod_spread = false;
		bool vsync = false;
		uint32_t windows = 0; // 0: continuous; else alternating animated / frozen windows of kWindowSeconds
		bool overview = false; // save s3_overview.png after the warm-up
	};
	struct Sample {
		float step_ms = 0, step_cpu_ms = 0, other_cpu_ms = 0, frame_ms = 0, render_cpu_ms = 0, render_gpu_ms = 0, job_ms = 0, blocked_ms = 0;
		uint32_t evaluated = 0, calls = 0;
		bool animated = true; // this frame's step ran
		bool prev_animated = true; // the previous frame's step ran (the class of other_cpu_ms)
	};
	struct RunState {
		RunSpec spec;
		double t = 0.0; // seconds since the run began
		uint64_t frames = 0;
		std::vector<Sample> samples;
		uint32_t in_view_min = UINT32_MAX;
		uint64_t blend_evaluated = 0, blend_two = 0, blend_idle = 0, blend_clips = 0;
		std::array<uint64_t, bf3::anim::TemporalLodTable::kTiers + 2> tiers{};
		double measured_seconds = 0.0;
		std::string vsync_read;
		bool overview_done = false;
	};
	struct ShotSpec {
		uint32_t team = 0; // 0 US (cell 3), 1 RU (cell 4)
		int pose = 0; // 0 idle, 1 run, 2 reload, 3 aim
		bool left_front = true;
	};

	void parse_args();
	void start_loader(); // after the session is done
	bool validate_stage(std::string *r_error); // main thread, own CollisionWorld (G0 stage row)
	bool build(std::string *r_error); // main thread, once the assets are ready
	bool make_evaluator(bool p_lod, std::string *r_error);
	bool make_uploader(SkinBackend p_backend, std::string *r_error);
	void step(double p_delta); // one frame of the protocol above
	uint32_t count_in_view() const; // Camera3D::is_position_in_frustum on every soldier box corner (G0 in-view row)
	void calibrate_cycles();
	double thread_ms() const; // main-thread CPU in ms (QueryThreadCycleTime / cycles_per_ms_)
	void begin_run(const RunSpec &p_spec);
	void check_frame(double p_delta, const Sample &p_sample, bool p_animated);
	CrowdRunStats summarize(const RunState &p_run, int p_class) const; // p_class: -1 all, 1 animated windows, 0 frozen
	HiddenCostStats hidden_cost(const RunState &p_run) const;
	bool shot_frame(); // G3: returns true when every shot is done
	void report_and_finish();
	void finish(int p_code);
	void shutdown(); // the order above; idempotent

	Bf3LevelSession *session_ = nullptr;
	godot::Camera3D *camera_ = nullptr;
	std::thread loader_;
	std::atomic<bool> assets_ready_{ false };
	std::string load_error_; // written by the loader before assets_ready_
	std::string cache_line_;
	std::shared_ptr<bf3::core::Cache> cache_; // acquire_cache on the loader thread
	std::filesystem::path cache_root_;
	bool no_cache_ = false;
	bf3::render::LoadProgress progress_;
	std::unique_ptr<bf3::render::SoldierAssets> assets_;
	std::unique_ptr<bf3::render::SoldierAssets> loaded_; // the loader's result (handed over after assets_ready_)
	std::unique_ptr<bf3::sim::CollisionWorld> world_;
	std::unique_ptr<bf3::anim::CrowdEvaluator> eval_;
	std::unique_ptr<bf3::core::WorkerGroup> lanes_;
	std::unique_ptr<SkinUploader> uploader_;
	std::vector<bf3::anim::CrowdTeam> teams_;
	bf3::anim::CrowdConfig config_;
	bf3::anim::CrowdStageSpec stage_;
	std::vector<bf3::anim::Vec3> feet_; // floor-snapped cell positions (validate_stage)
	std::vector<bf3::anim::CrowdSoldierInput> inputs_;
	std::vector<godot::Transform3D> roots_;
	std::vector<uint8_t> team_of_;
	SkinUploadStats upload_stats_;
	double stage_time_ = 0.0;
	double cycles_per_ms_ = 0.0; // QueryThreadCycleTime calibration
	double cycle_selftest_ratio_ = 0.0;
	SkinBackend backend_ = SkinBackend::RsSkeleton;
	bool lod_ = true;
	bool lod_spread_ = false; // R3: stage_input reports kLodRowDistance
	bool frozen_ = false; // step not called this frame
	bool check_ = false;
	bool walkers_zero_ = false; // --walkers=0 given (the session's arg)
	bool no_hud_ = false; // --no-hud given (the session's arg)
	double run_seconds_ = 30.0;
	std::string shot_dir_;
	bool shot_dir_ok_ = false; // outside the install and created (build)
	// Run state.
	bool started_ = false; // the loader was started
	bool built_ = false;
	bool failed_ = false;
	bool finished_ = false;
	bool skip_ = false; // --s3-check on a screen that is not 2560x1440: exit 77 on the first frame
	bool shut_down_ = false;
	uint64_t last_entry_usec_ = 0;
	double last_entry_cpu_ms_ = 0.0;
	double prev_step_cpu_ms_ = 0.0;
	bool prev_animated_ = true;
	// The check.
	std::vector<std::string> report_;
	bool g0_stage_ = false, g0_config_ = false;
	std::string g0_config_detail_;
	std::string stage_error_;
	std::vector<RunSpec> plan_;
	size_t plan_index_ = 0;
	std::unique_ptr<RunState> run_;
	std::vector<RunState> done_runs_;
	std::array<double, 2> bench_metric_{ { -1.0, -1.0 } }; // A, B
	SkinBackend winner_ = SkinBackend::RsSkeleton;
	bool shots_started_ = false;
	std::vector<ShotSpec> shots_;
	size_t shot_index_ = 0;
	int shot_wait_ = 0;
	uint32_t shots_ok_ = 0;
	std::vector<std::string> shot_lines_;
	bool viewport_ok_ = false;
	bool other_zero_ = true;
	bool vsync_ok_ = true;
	int settle_frames_ = 0;
};

} // namespace bf3::godot_side
