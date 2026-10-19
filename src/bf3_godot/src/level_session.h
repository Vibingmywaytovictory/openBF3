#pragma once
// Bf3LevelSession: the M1 session node, "Metro walkable through the loopback server" (plan §6 M1; docs/design/m1.md
// §11, §12). Scene: res://scenes/m1_metro.tscn.
//
// It owns, for one level x mode:
//   * the loader thread (std::thread, no Godot API): Phase A of the load (mount, partitions, traversal, census, scene
//     plan, SceneLanes::start, then spawns, soldier and static world through the cache: level_load.h), Phase D (the
//     ServerHost with a GameServer and the ClientSession, both started as soon as the SharedLevel exists, so the
//     soldier spawns while the meshes still load), then the wait for the scene lanes (Phase B) and the release of the
//     level database;
//   * the scene lanes (bf3_render SceneLanes: 8 lanes, 6 of them for textures, 256 MiB in flight) and the upload
//     (level_upload.h: C1 on Godot worker tasks, C2 on the main thread with 12 ms per frame);
//   * the cameras (fp_camera.h), the input mapper (input_mapper.h), the loading screen (loading_screen.h), the sun
//     and the environment from the plan (docs §11.4), the others drawn as capsules (present_others per display frame),
//     a small HUD;
//   * the checks (m1_check.h).
// Thread counts (docs §3, §12.2): 8 scene lanes + 8 shape threads on 16 cores (half each on fewer); Godot's pool is
// capped at 4 in project.godot and C1 keeps at most 3 tasks in flight.
//
// The headless skip (docs §11.1): under --headless (the dummy renderer) or --m1-check the session does no scene work
// at all (no plan, no lanes, no Phase C): the dummy renderer would keep CPU copies of every texture and mesh. It
// loads the database, the shapes, the spawns and the motor (load_shared_level), the path M3's dedicated server takes.
//
// Vsync: the loading screen turns vsync off (DisplayServer::window_set_vsync_mode) and the frame cap to 0
// (Engine::set_max_fps) so the main thread's C2 slices are not stretched by the display; both are restored at Done.
//
// LoadStage::Done is the first frame with the own soldier spawned, its spawn view adopted by the mapper and a valid
// presentation, after the upload finished (no scene: once spawned). The loading screen goes then.
//
// Lifetime: NOTIFICATION_PREDELETE cancels the lanes, joins the loader, waits for the C1 tasks, stops the client,
// then the server, and frees the RenderingServer objects. Extension deinit calls ClientSession::stop_all and
// ServerHost::stop_all (register_types.cpp).
//
// User args (after "--"):
//   --level=MP_Subway --mode=TeamDeathMatch0   level x mode (the defaults)
//   --netsim=<profile>                          the client's netsim (net/netsim.h; default off)
//   --walkers=<n>                               scripted walkers on the server (default 2)
//   --seed=<n>                                  server and netsim seed (default 1)
//   --bf3-cache=<dir> / --no-cache              cache root (default user://bf3cache) / convert without a cache
//   --bf3-install=<dir>                         install override, as for GameSession
//   --m1-check                                  headless E1/E2/E3 (m1_check.h); PASS/FAIL table, quits 0 or 1
//   --m1-courses=<n>                            E1 courses through the full path (default 6, as sim.motor)
//   --m1-load-check=cold|warm                   windowed E4 / E5 (and the game's peak working set); quits 0 or 1
//   --shots=<png>@x,y,z,yaw,pitch;...           free-camera screenshots after Done (degrees; sim yaw: 0 looks
//                                               down +Z, 90 down +X; pitch + up)
//   --shot-spawns=<i>[:stand|crouch|prone|walk],...  first-person screenshots at spawn points (SpawnAt)
//   --shot-dir=<dir>                            where --shot-spawns writes spawn_<i>_<pose>.png
//   --shot-settle=<s> / --shot-measure=<n>      L1 look: free shots wait s seconds (auto exposure), then time n frames
//   --quit-after=<s>                            quit s seconds after Done
//   --no-hud                                    hide the HUD
//   --fog                                       M1 look: draw the plan's depth fog (off by default: see build_environment)
//   --look=l1|m1                                L1 look (default; README "L1 look") or the M1 look (before / after
//                                               shots and the godot.l1_look frame-time A/B)
//   --look-off=exposure,fog,bloom,lut,blend     L1 look diagnosis: drop parts (exposure: the static estimate; blend:
//                                               glass draws opaque)
//   --look-bloom                                L1 look: draw BF3's bloom (off by default: README "L1 look")
//   --look-exposure=<e>                         L1 look diagnosis: a fixed tonemap exposure instead of auto exposure
// Controls: WASD move, Shift sprint, Space jump, Ctrl crouch, Z prone, mouse look, F6 free camera (Q/E down/up,
// wheel speed), Esc releases the mouse, a click captures it.
//
// S1 (docs/design/s1.md §12), user args:
//   --bots=<n>                                  n walker bots on the server (0..120; with them the static world carries
//                                               the terrain cover and the water rule, the loader bakes the navmesh)
//   --nav-overlay                               draw the navmesh (nav_overlay.h); F7 toggles
//   --shot-delay=<s>                            take the --shots s seconds after Done (the bots spread out first)

#include "fp_camera.h"
#include "input_mapper.h"
#include "level_upload.h"
#include "loading_screen.h"
#include "look_bridge.h"
// S1
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
class CapsuleMesh;
class DirectionalLight3D;
class Label;
class WorldEnvironment;
} // namespace godot

namespace bf3::core {
class Cache;
class Install;
} // namespace bf3::core

// S1
namespace bf3::host {
struct BotLevel;
} // namespace bf3::host

namespace bf3::godot_side {

class M1Check;
class M1LoadCheck;
class ShotRunner;

// Phase timings of one load, steady_clock microseconds (level_load.h's clock) unless named otherwise.
struct SessionTimings {
	uint64_t ready_us = 0; // _ready
	double process_start_to_ready_s = 0.0; // the process's creation time (GetProcessTimes) -> _ready
	uint64_t plan_us = 0; // the scene plan was published
	uint64_t shared_us = 0; // the SharedLevel was published
	uint64_t session_us = 0; // server and client started
	uint64_t lanes_us = 0; // SceneLanes::wait returned
	uint64_t upload_us = 0; // the upload finished (main thread saw it)
	uint64_t done_us = 0; // LoadStage::Done
	double done_from_process_s = 0.0; // process creation -> Done (E4 / E5, orchestrator decision on docs §16 q4)
	double done_from_ready_s = 0.0; // _ready -> Done (reported)
	double seconds_census = 0.0;
	double seconds_plan = 0.0;
	double seconds_cache_open = 0.0;
	uint64_t loading_frames = 0; // frames with the loading screen up
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

	// ---- for the checks (main thread) -------------------------------------------------------------------------
	bool done() const { return done_; }
	bool scene_enabled() const { return scene_; }
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
	// Stops the client and starts a new one with this netsim profile (E2 runs); the mapper forgets the adopted life.
	bool restart_client(const std::string &p_netsim, uint64_t p_seed, std::string *r_error);
	// Saves the viewport to p_path (refused inside the install). Main thread.
	bool save_screenshot(const std::string &p_path);
	// Ends the run with an exit code (prints install.unchanged, joins and quits).
	void finish(int p_code);

	// The C1 task body (WorkerThreadPool; through callable_mp).
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
	bool start_session(std::string *r_error); // loader thread
	bf3::host::ClientSessionConfig client_config(const std::string &p_netsim, uint64_t p_seed) const;

	// Args.
	std::string level_ = "MP_Subway";
	std::string mode_ = "TeamDeathMatch0";
	std::string netsim_ = "off";
	uint32_t walkers_ = 2;
	uint64_t seed_ = 1;
	bool no_cache_ = false;
	std::filesystem::path cache_root_;
	bool m1_check_ = false;
	std::string load_check_; // "", "cold", "warm"
	double quit_after_ = 0.0;
	bool hud_enabled_ = true;
	bool headless_ = false;
	bool scene_ = true;
	bf3::render::SceneLook look_mode_ = bf3::render::SceneLook::L1; // --look

	// Install.
	bf3::core::InstallReader install_;
	bf3::core::InstallSnapshot install_before_;
	bf3::core::PathGuard guard_;
	std::string motor_tuning_text_;
	std::string motor_tuning_source_;

	// Loader thread state (published with release stores; read by the main thread after the acquire loads).
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

	// Session.
	std::unique_ptr<bf3::host::ServerHost> server_;
	std::unique_ptr<bf3::host::ClientSession> client_;
	std::unique_ptr<bf3::host::PresentationFrame> frame_;

	// Godot side.
	LevelUpload upload_;
	godot::Callable upload_task_;
	InputMapper mapper_;
	FpCamera cameras_;
	LoadingScreen loading_;
	godot::DirectionalLight3D *sun_ = nullptr;
	godot::WorldEnvironment *environment_ = nullptr;
	LookBridge look_; // L1
	godot::Ref<godot::CapsuleMesh> capsule_;
	std::vector<godot::RID> other_instances_;
	godot::Label *hud_ = nullptr;
	double hud_timer_ = 0.0;
	bool loading_display_ = false;
	int saved_vsync_ = -1;
	int saved_max_fps_ = 0;

	// Run state.
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
	std::unique_ptr<M1Check> check_;
	std::unique_ptr<M1LoadCheck> load_check_runner_;
	std::unique_ptr<ShotRunner> shots_;

	// S1 (docs/design/s1.md §12): walker bots and the nav overlay.
	void s1_upload_overlay(); // main thread
	void s1_report_others(uint64_t p_now_ns); // main thread
	uint32_t bots_ = 0;
	bool nav_overlay_ = false;
	double shot_delay_ = 0.0;
	std::shared_ptr<const bf3::host::BotLevel> bot_level_; // loader thread until session_ready_
	NavOverlayArrays nav_arrays_; // written by the loader before nav_ready_ (release)
	std::atomic<bool> nav_ready_{ false };
	NavOverlay nav_draw_;
	std::string bots_line_; // written by the loader before session_ready_
	bool s1_reported_ = false;

	// M2a w3 [G7] (W3.0 contract, docs/design/m2a-w3.md; owner gx): the wave-3 hooks (level_session_m2a.cpp) and the
	// state they own (M2aSession, level_session_m2a.h). Each hook is called from one tagged line in
	// level_session.cpp; W3.0's bodies do nothing, so M1, S1 and S3 behave exactly as before.
	struct M2aSession;
	struct M2aSessionDeleter {
		void operator()(M2aSession *p_session) const;
	};
	void m2a_parse_args(); // end of parse_args
	void m2a_load_options(bf3::host::SharedLoadOptions &r_options); // run_loader: before load_shared_level
	// run_loader: after the S1 bot level, before Phase D (shared_ is set); false fails the load with *r_error.
	bool m2a_load_assets(const bf3::host::LevelDatabase &p_db, bf3::core::Cache *p_cache, unsigned p_threads, std::string *r_error);
	void m2a_server_config(bf3::host::GameServerConfig &r_game); // start_session: before the ServerConfig
	void m2a_client_config(bf3::host::ClientSessionConfig &r_config) const; // client_config: before it returns
	void m2a_presented(double p_delta, uint64_t p_now_ns); // _process: every frame after set_input (drains events)
	bool m2a_frame(double p_delta, uint64_t p_now_ns); // _process after Done, after the S1 calls; true: frame handled
	void m2a_input(const godot::Ref<godot::InputEvent> &p_event); // _unhandled_input: after the mapper
	void m2a_shutdown(); // shutdown: after the loader joined, before the client stops
	bool m2b_join_ready() const; // M2b gx [done]: try_done's spawn screen at join (level_session_m2b.cpp)
	std::unique_ptr<M2aSession, M2aSessionDeleter> m2a_;
};

} // namespace bf3::godot_side
