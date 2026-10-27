#pragma once

#include "fp_camera.h"
#include "input_mapper.h"
#include "level_upload.h"
#include "look_bridge.h"
#include "native_loading.h"

#include "nav_overlay.h"

#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/host/server_host.h"
#include "bf3/render/level_scene.h"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Camera3D;
class CapsuleMesh;
class DirectionalLight3D;
class Label;
class WorldEnvironment;
}

namespace bf3::core {
class Cache;
class Install;
}

namespace bf3::host {
struct BotLevel;
}

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::godot_side {

class Bf3UiAssets;
class Bf3UiScreens;
class M1Check;
class M1LoadCheck;
class ShotRunner;
class UiScreenRecords;

struct SessionTimings {
	uint64_t ready_us = 0;
	double process_start_to_ready_s = 0.0;
	uint64_t plan_us = 0;
	uint64_t shared_us = 0;
	uint64_t session_us = 0;
	uint64_t lanes_us = 0;
	uint64_t upload_us = 0;
	uint64_t done_us = 0;
	double done_from_process_s = 0.0;
	double done_from_ready_s = 0.0;
	double seconds_census = 0.0;
	double seconds_plan = 0.0;
	double seconds_cache_open = 0.0;
	uint64_t loading_frames = 0;
	double loading_frame_ms_sum = 0.0;
	double loading_frame_ms_max = 0.0;
};

class Bf3LevelSession : public godot::Node3D {
	GDCLASS(Bf3LevelSession, godot::Node3D)

public:
	Bf3LevelSession();
	~Bf3LevelSession() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _unhandled_input(const godot::Ref<godot::InputEvent> &p_event) override;

	godot::String get_status_text() const;

	bool done() const { return done_; }
	bool scene_enabled() const { return scene_; }
	bool game_session() const { return game_ != nullptr; }
	bool failed() const { return failed_; }
	const std::string &level_id() const { return level_; }
	const std::string &mode_id() const { return mode_; }
	const bf3::core::InstallReader &install() const { return install_; }
	const bf3::core::InstallSnapshot &install_snapshot_at_start() const { return install_before_; }
	std::shared_ptr<const bf3::host::SharedLevel> shared_level() const;
	std::shared_ptr<const bf3::render::LevelScenePlan> scene_plan() const;
	bf3::host::ServerHost *server() const { return server_.get(); }
	bf3::host::ClientSession *client() const { return client_.get(); }
	const bf3::host::PresentationFrame &frame() const { return *frame_; }
	InputMapper &mapper() { return mapper_; }
	FpCamera &cameras() { return cameras_; }
	const bf3::host::LoadProgress &progress() const { return progress_; }
	const SessionTimings &timings() const { return timings_; }
	const LevelUpload &upload() const { return upload_; }
	std::optional<bf3::render::SceneLaneStats> lane_stats() const;
	std::string cache_summary() const;
	std::string look_summary() const { return look_.built() ? look_.describe() : std::string("look M1"); }
	const std::vector<float> &frame_ms_after_done() const { return frame_ms_after_done_; }
	uint64_t others_presented() const { return others_presented_; }
	uint64_t others_extrapolated() const { return others_extrapolated_; }
	void reset_others_counts() { others_presented_ = others_extrapolated_ = 0; }

	const bf3::sim::CollisionWorld *collision_world() const;
	Bf3UiAssets *ui_assets() const;
	const Bf3UiScreens *screen_stack() const;
	const UiScreenRecords *screen_records() const;
	void scripted_ui_key(godot::Key p_key, bool p_pressed);

	bool restart_client(const std::string &p_netsim, uint64_t p_seed, std::string *r_error);

	bool save_screenshot(const std::string &p_path);

	void finish(int p_code);

	void _upload_task();

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void parse_args();
	void run_loader();
	void shutdown();
	void on_plan_ready();
	void build_environment(const bf3::render::SceneEnvironment &p_env);
	void build_hud();
	void update_hud(double p_delta);
	void draw_others(uint64_t p_now_ns);
	void try_done();
	void set_loading_display(bool p_loading);
	bool start_session(std::string *r_error);
	bf3::host::ClientSessionConfig client_config(const std::string &p_netsim, uint64_t p_seed) const;

	std::string level_ = "MP_Subway";
	std::string mode_ = "TeamDeathMatch0";
	std::string netsim_ = "off";
	uint32_t walkers_ = 2;
	uint64_t seed_ = 1;
	bool no_cache_ = false;
	std::filesystem::path cache_root_;
	bool m1_check_ = false;
	std::string load_check_;
	double quit_after_ = 0.0;
	bool hud_enabled_ = true;
	bool headless_ = false;
	bool scene_ = true;
	bf3::render::SceneLook look_mode_ = bf3::render::SceneLook::L1;

	bf3::core::InstallReader install_;
	bf3::core::InstallSnapshot install_before_;
	bf3::core::PathGuard guard_;
	std::string motor_tuning_text_;
	std::string motor_tuning_source_;

	std::thread loader_;
	std::atomic<bool> stop_{ false };
	std::atomic<bool> loader_done_{ false };
	std::atomic<bool> plan_ready_{ false };
	std::atomic<bool> shared_ready_{ false };
	std::atomic<bool> session_ready_{ false };
	std::atomic<bool> load_failed_{ false };
	mutable std::mutex loader_mutex_;
	std::string load_error_;
	std::string cache_line_;
	std::shared_ptr<const bf3::render::LevelScenePlan> plan_;
	std::shared_ptr<const bf3::host::SharedLevel> shared_;
	std::optional<bf3::render::SceneLaneStats> lane_stats_;
	std::unique_ptr<bf3::render::SceneLanes> lanes_;
	bf3::host::LoadProgress progress_;

	std::unique_ptr<bf3::host::ServerHost> server_;
	std::unique_ptr<bf3::host::ClientSession> client_;
	std::unique_ptr<bf3::host::PresentationFrame> frame_;

	LevelUpload upload_;
	godot::Callable upload_task_;
	InputMapper mapper_;
	FpCamera cameras_;
	NativeLoading loading_;
	godot::DirectionalLight3D *sun_ = nullptr;
	godot::WorldEnvironment *environment_ = nullptr;
	LookBridge look_;
	godot::Ref<godot::CapsuleMesh> capsule_;
	std::vector<godot::RID> other_instances_;
	godot::Label *hud_ = nullptr;
	double hud_timer_ = 0.0;
	bool loading_display_ = false;
	int saved_vsync_ = -1;
	int saved_max_fps_ = 0;

	bool done_ = false;
	bool failed_ = false;
	bool quitting_ = false;
	bool shut_down_ = false;
	double since_done_ = 0.0;
	uint64_t frames_ = 0;
	std::vector<float> frame_ms_after_done_;
	uint64_t others_presented_ = 0;
	uint64_t others_extrapolated_ = 0;
	SessionTimings timings_;
#if OPENBF3_BUILD_CHECKS
	std::unique_ptr<M1Check> check_;
	std::unique_ptr<M1LoadCheck> load_check_runner_;
	std::unique_ptr<ShotRunner> shots_;
#endif

	void upload_nav_overlay();
	void report_bots(uint64_t p_now_ns);
	uint32_t bots_ = 0;
	bool nav_overlay_ = false;
	double shot_delay_ = 0.0;
	std::shared_ptr<const bf3::host::BotLevel> bot_level_;
	NavOverlayArrays nav_arrays_;
	std::atomic<bool> nav_ready_{ false };
	NavOverlay nav_draw_;
	std::string bots_line_;
	bool bots_reported_ = false;

	struct Game;
	struct GameDeleter {
		void operator()(Game *p_game) const;
	};
	void game_parse_args();
	void game_load_options(bf3::host::SharedLoadOptions &r_options);
	bool game_load_assets(const bf3::host::LevelDatabase &p_db, bf3::core::Cache *p_cache, unsigned p_threads, std::string *r_error);
	void game_server_config(bf3::host::GameServerConfig &r_game);
	void game_client_config(bf3::host::ClientSessionConfig &r_config) const;
	void game_presented(double p_delta, uint64_t p_now_ns);
	bool game_frame(double p_delta, uint64_t p_now_ns);
	void game_shutdown();

	void tdm_parse_args(std::string &r_error);
	void tdm_load_options(bf3::host::SharedLoadOptions &r_options);
	void tdm_server_config(bf3::host::GameServerConfig &r_config);
	void tdm_presented(uint64_t p_now_ns);
	bool tdm_frame(uint64_t p_now_ns, double p_delta, godot::Camera3D *p_camera, bool p_free_camera);
	void tdm_shutdown();
	void tdm_ui_input(const godot::Ref<godot::InputEvent> &p_event);
	bool tdm_join_ready() const;
	std::unique_ptr<Game, GameDeleter> game_;
};

}
