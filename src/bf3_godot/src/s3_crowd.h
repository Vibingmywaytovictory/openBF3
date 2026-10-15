#pragma once
// Bf3Crowd: the S3 crowd node (docs/design/s3.md). Scene res://scenes/s3_crowd.tscn = Node3D root > Session
// (Bf3LevelSession, Metro TDM, run with --walkers=0) + Crowd (Bf3Crowd). The node only reads the session
// through its public accessors (done(), shared_level(), install(), cameras(), save_screenshot(), finish()),
// so no M1 file changes.
//
// Once the session reports done() and a loader thread has run bf3::render::load_soldier_assets, the node:
//  * builds 64 soldiers (32 US / 32 RU) on the stage grid (bf3::anim::CrowdStageSpec) at the own soldier's
//    spawn, with its own Camera3D framing the grid (3 m behind and 1.7 m above the front row);
//  * per frame (_process, main thread): G1 timer start -> CrowdEvaluator::wait_and_publish -> SkinUploader::upload
//    (roots + changed palettes) -> set_inputs (stage_input) -> kick -> G1 timer stop. The pose job runs on a
//    bf3::core::WorkerGroup of min(6, cores - 4) lanes (at least 2) while the frame renders.
//
// User args (after "--"):
//   --s3-crowd                  run the crowd (default in the scene; without it the scene is plain M1)
//   --s3-backend=<rs|skeleton3d|texture>   skin upload backend (default: the s3.md §7.3 choice, rs)
//   --s3-lod=<on|off>           temporal LOD (default on)
//   --s3-check                  the gate run (s3.md §1): window 2560x1440 borderless at the primary screen's
//                               origin, backend bench, G1 / G2 runs, G3 shots and metrics; prints a table and
//                               "S3 CHECK: PASS|FAIL"; exits 0 / 1, or 77 when the primary screen is not 2560x1440
//                               or no rendering device exists
//   --s3-seconds=<s>            length of each measured run (default and minimum for the gate: 30)
//   --shot-dir=<dir>            G3 shots (default <repo>/b/shots/s3; refused inside the install)
//   --quit-after=<s>            as for the session (always passed by windowed runs)

#include "skin_upload.h"

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/crowd_stage.h"
#include "bf3/core/worker_group.h"
#include "bf3/render/soldier_assets.h"

#include <godot_cpp/classes/node3d.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Camera3D;
}

namespace bf3::godot_side {

class Bf3LevelSession;

// One measured run of the check (s3.md §1, §9.3).
struct CrowdRunStats {
	std::string label; // e.g. "rs lod=off vsync=off"
	uint32_t frames = 0;
	double seconds = 0.0;
	double main_p50_ms = 0.0, main_p99_ms = 0.0; // G1 timer
	double frame_p50_ms = 0.0, frame_p99_ms = 0.0; // G2: _process delta
	double render_cpu_p50_ms = 0.0, render_gpu_p50_ms = 0.0; // viewport measured render times
	double job_p50_ms = 0.0, job_p99_ms = 0.0; // worker job wall time
	double blocked_p99_ms = 0.0; // main thread waiting on the job
	double evaluated_mean = 0.0; // soldiers posed per frame
	uint32_t calls_p50 = 0; // Godot calls per frame by the uploader
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
	void build(); // main thread, once the assets are ready
	void step(double p_delta); // one frame of the protocol above
	void shutdown(); // waits for the job, frees uploader resources, joins the loader

	Bf3LevelSession *session_ = nullptr;
	godot::Camera3D *camera_ = nullptr;
	std::thread loader_;
	std::atomic<bool> assets_ready_{ false };
	std::unique_ptr<bf3::render::SoldierAssets> assets_;
	std::unique_ptr<bf3::anim::CrowdEvaluator> eval_;
	std::unique_ptr<bf3::core::WorkerGroup> lanes_;
	std::unique_ptr<SkinUploader> uploader_;
	bf3::anim::CrowdStageSpec stage_;
	std::vector<bf3::anim::CrowdSoldierInput> inputs_;
	std::vector<uint8_t> team_of_;
	double stage_time_ = 0.0;
	SkinBackend backend_ = SkinBackend::RsSkeleton;
	bool lod_ = true;
	bool check_ = false;
	double run_seconds_ = 30.0;
	std::string shot_dir_;
};

} // namespace bf3::godot_side
