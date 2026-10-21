#pragma once

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

struct CrowdRunStats {
	std::string label;
	std::string vsync_mode;
	uint32_t frames = 0;
	double seconds = 0.0;
	double fps = 0.0;
	double step_p50_ms = 0.0, step_p99_ms = 0.0;
	double step_cpu_p50_ms = 0.0;
	double other_cpu_p50_ms = 0.0;
	double frame_p50_ms = 0.0, frame_p99_ms = 0.0;
	double render_cpu_p50_ms = 0.0, render_gpu_p50_ms = 0.0;
	double job_p50_ms = 0.0, job_p99_ms = 0.0;
	double blocked_p99_ms = 0.0;
	double evaluated_mean = 0.0;
	std::array<double, bf3::anim::TemporalLodTable::kTiers + 2> evaluated_by_tier_per_s{};
	uint32_t calls_p50 = 0;

	uint32_t in_view_min = 0;
	double two_cycle_fraction = 0.0, idle_mix_fraction = 0.0, mean_loco_clips = 0.0;
};

struct HiddenCostStats {
	CrowdRunStats animated;
	CrowdRunStats frozen;
	double hidden_p50_ms = 0.0;
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

	struct RunSpec {
		std::string label;
		SkinBackend backend = SkinBackend::RsSkeleton;
		bool lod = false;
		bool lod_spread = false;
		bool vsync = false;
		uint32_t windows = 0;
		bool overview = false;
		bool palettes = true;
		bool crowd_hidden = false;
	};
	struct Sample {
		float step_ms = 0, step_cpu_ms = 0, other_cpu_ms = 0, frame_ms = 0, render_cpu_ms = 0, render_gpu_ms = 0, job_ms = 0, blocked_ms = 0;
		uint32_t evaluated = 0, calls = 0;
		bool animated = true;
		bool prev_animated = true;
	};
	struct RunState {
		RunSpec spec;
		double t = 0.0;
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
		uint32_t team = 0;
		int pose = 0;
		bool left_front = true;
	};

	void parse_args();
	void start_loader();
	bool validate_stage(std::string *r_error);
	bool build(std::string *r_error);
	bool make_evaluator(bool p_lod, std::string *r_error);
	bool make_uploader(SkinBackend p_backend, std::string *r_error);
	void step(double p_delta);
	uint32_t count_in_view() const;
	void calibrate_cycles();
	double thread_ms() const;
	void begin_run(const RunSpec &p_spec);
	void check_frame(double p_delta, const Sample &p_sample, bool p_animated);
	CrowdRunStats summarize(const RunState &p_run, int p_class) const;
	HiddenCostStats hidden_cost(const RunState &p_run) const;
	bool shot_frame();
	void report_and_finish();
	void finish(int p_code);
	void shutdown();

	Bf3LevelSession *session_ = nullptr;
	godot::Camera3D *camera_ = nullptr;
	std::thread loader_;
	std::atomic<bool> assets_ready_{ false };
	std::string load_error_;
	std::string cache_line_;
	std::shared_ptr<bf3::core::Cache> cache_;
	std::filesystem::path cache_root_;
	bool no_cache_ = false;
	bf3::render::LoadProgress progress_;
	std::unique_ptr<bf3::render::SoldierAssets> assets_;
	std::unique_ptr<bf3::render::SoldierAssets> loaded_;
	std::unique_ptr<bf3::sim::CollisionWorld> world_;
	std::unique_ptr<bf3::anim::CrowdEvaluator> eval_;
	std::unique_ptr<bf3::core::WorkerGroup> lanes_;
	std::unique_ptr<SkinUploader> uploader_;
	std::vector<bf3::anim::CrowdTeam> teams_;
	bf3::anim::CrowdConfig config_;
	bf3::anim::CrowdStageSpec stage_;
	std::vector<bf3::anim::Vec3> feet_;
	std::vector<bf3::anim::CrowdSoldierInput> inputs_;
	std::vector<godot::Transform3D> roots_;
	std::vector<uint8_t> team_of_;
	SkinUploadStats upload_stats_;
	double stage_time_ = 0.0;
	double cycles_per_ms_ = 0.0;
	double cycle_selftest_ratio_ = 0.0;
	double single_thread_ms_ = 0.0;
	SkinBackend backend_ = SkinBackend::RsSkeleton;
	bool lod_ = true;
	bool lod_spread_ = false;
	bool frozen_ = false;
	bool check_ = false;
	bool diag_ = false;
	bool walkers_zero_ = false;
	bool no_hud_ = false;
	double run_seconds_ = 30.0;
	std::string shot_dir_;
	bool shot_dir_ok_ = false;

	bool started_ = false;
	bool built_ = false;
	bool failed_ = false;
	bool finished_ = false;
	bool skip_ = false;
	bool shut_down_ = false;
	uint64_t last_entry_usec_ = 0;
	double last_entry_cpu_ms_ = 0.0;
	double prev_step_cpu_ms_ = 0.0;
	bool prev_animated_ = true;

	std::vector<std::string> report_;
	bool g0_stage_ = false, g0_config_ = false;
	std::string g0_config_detail_;
	std::string stage_error_;
	std::vector<RunSpec> plan_;
	size_t plan_index_ = 0;
	std::unique_ptr<RunState> run_;
	std::vector<RunState> done_runs_;
	std::array<double, 2> bench_metric_{ { -1.0, -1.0 } };
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

}
