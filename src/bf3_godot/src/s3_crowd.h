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
// self-test (a 100 ms busy loop must read 100 +- 5 ms of thread CPU; else G0 config FAIL). Under the rendering
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
//   --s3-crowd                  run the crowd (default in the scene; without it the scene is plain M1)
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
	void parse_args();
	void start_loader(); // after the session is done
	bool validate_stage(std::string *r_error); // main thread, own CollisionWorld (G0 stage row)
	void build(); // main thread, once the assets are ready
	void step(double p_delta); // one frame of the protocol above
	uint32_t count_in_view() const; // Camera3D::is_position_in_frustum on every soldier box corner (G0 in-view row)
	void shutdown(); // the order above; idempotent

	Bf3LevelSession *session_ = nullptr;
	godot::Camera3D *camera_ = nullptr;
	std::thread loader_;
	std::atomic<bool> assets_ready_{ false };
	std::shared_ptr<bf3::core::Cache> cache_; // acquire_cache on the loader thread
	std::filesystem::path cache_root_;
	std::unique_ptr<bf3::render::SoldierAssets> assets_;
	std::unique_ptr<bf3::sim::CollisionWorld> world_;
	std::unique_ptr<bf3::anim::CrowdEvaluator> eval_;
	std::unique_ptr<bf3::core::WorkerGroup> lanes_;
	std::unique_ptr<SkinUploader> uploader_;
	bf3::anim::CrowdStageSpec stage_;
	std::vector<bf3::anim::Vec3> feet_; // floor-snapped cell positions (validate_stage)
	std::vector<bf3::anim::CrowdSoldierInput> inputs_;
	std::vector<uint8_t> team_of_;
	double stage_time_ = 0.0;
	double cycles_per_ms_ = 0.0; // QueryThreadCycleTime calibration
	SkinBackend backend_ = SkinBackend::RsSkeleton;
	bool lod_ = true;
	bool lod_spread_ = false; // R3: stage_input reports kLodRowDistance
	bool frozen_ = false; // step not called this frame
	bool check_ = false;
	bool walkers_zero_ = false; // --walkers=0 given (the session's arg)
	bool no_hud_ = false; // --no-hud given (the session's arg)
	double run_seconds_ = 30.0;
	std::string shot_dir_;
};

} // namespace bf3::godot_side
