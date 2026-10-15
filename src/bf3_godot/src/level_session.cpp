#include "level_session.h"

#include "godot_cache.h"
#include "godot_util.h"
#include "m1_check.h"

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install.h"
#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/host/game_server.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/net/netsim.h"
// S1
#include "bf3/core/level/mode_areas.h"
#include "bf3/host/bot_fill.h"
#include "bf3/sim/nav.h"
#include <godot_cpp/classes/input_event_key.hpp>

#include <godot_cpp/classes/capsule_mesh.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/procedural_sky_material.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// psapi after windows.h
#include <psapi.h>

namespace bf3::godot_side {

using namespace godot;
namespace host = bf3::host;
namespace lvl = bf3::core::level;
namespace render = bf3::render;
namespace sim = bf3::sim;

namespace {

uint64_t now_us() {
	return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Seconds since this process was created (GetProcessTimes), the start of E4 / E5 (orchestrator decision on docs §16
// question 4: process start -> first controllable frame).
double seconds_since_process_creation() {
	FILETIME creation{}, exit_time{}, kernel{}, user{};
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) {
		return 0.0;
	}
	FILETIME now{};
	GetSystemTimePreciseAsFileTime(&now);
	const uint64_t c = (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
	const uint64_t n = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
	return n > c ? double(n - c) / 1e7 : 0.0;
}

uint64_t peak_working_set_bytes() {
	PROCESS_MEMORY_COUNTERS pmc{};
	pmc.cb = sizeof(pmc);
	return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? uint64_t(pmc.PeakWorkingSetSize) : 0;
}

double mib(uint64_t p_bytes) {
	return double(p_bytes) / (1024.0 * 1024.0);
}

std::filesystem::path utf8_path(const std::string &p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

Color to_color(const std::array<float, 3> &p_c) {
	return Color(p_c[0], p_c[1], p_c[2]);
}

} // namespace

Bf3LevelSession::Bf3LevelSession() :
		frame_(std::make_unique<host::PresentationFrame>()) {}

Bf3LevelSession::~Bf3LevelSession() {
	shutdown();
}

void Bf3LevelSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_status_text"), &Bf3LevelSession::get_status_text);
}

void Bf3LevelSession::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		shutdown();
	}
}

void Bf3LevelSession::shutdown() {
	if (shut_down_) {
		return;
	}
	shut_down_ = true;
	stop_.store(true, std::memory_order_release);
	if (lanes_ != nullptr) {
		lanes_->cancel(); // a lane waiting for in-flight bytes wakes up; items not started are skipped
	}
	if (loader_.joinable()) {
		loader_.join();
	}
	if (WorkerThreadPool::get_singleton() != nullptr) {
		upload_.wait_tasks();
	}
	if (client_ != nullptr) {
		client_->stop();
	}
	if (server_ != nullptr) {
		server_->stop();
	}
	upload_.set_lanes(nullptr); // the upload never sees a freed SceneLanes
	lanes_.reset();
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (const RID &r : other_instances_) {
			rs->free_rid(r);
		}
		other_instances_.clear();
		upload_.free_all();
		nav_draw_.free_all(); // S1
	}
	if (loading_display_) {
		set_loading_display(false);
	}
}

void Bf3LevelSession::parse_args() {
	if (!user_arg_value("level").empty()) {
		level_ = user_arg_value("level");
	}
	if (!user_arg_value("mode").empty()) {
		mode_ = user_arg_value("mode");
	}
	if (!user_arg_value("netsim").empty()) {
		netsim_ = user_arg_value("netsim");
	}
	if (has_user_arg("walkers")) {
		walkers_ = uint32_t(std::clamp(std::atoi(user_arg_value("walkers").c_str()), 0, 16));
	}
	if (has_user_arg("seed")) {
		seed_ = uint64_t(std::strtoull(user_arg_value("seed").c_str(), nullptr, 10));
	}
	no_cache_ = has_user_arg("no-cache");
	if (!no_cache_) {
		const std::string root = user_arg_value("bf3-cache");
		cache_root_ = root.empty() ? default_cache_root() : utf8_path(root);
	}
	m1_check_ = has_user_arg("m1-check");
	if (has_user_arg("m1-load-check")) {
		load_check_ = user_arg_value("m1-load-check");
		if (load_check_.empty()) {
			load_check_ = "cold";
		}
	}
	const std::string quit_after = user_arg_value("quit-after");
	quit_after_ = quit_after.empty() ? 0.0 : std::atof(quit_after.c_str());
	hud_enabled_ = !has_user_arg("no-hud");
	// S1 (docs/design/s1.md §12).
	if (has_user_arg("bots")) {
		bots_ = uint32_t(std::clamp(std::atoi(user_arg_value("bots").c_str()), 0, 120));
	}
	nav_overlay_ = bots_ > 0 && has_user_arg("nav-overlay");
	const std::string shot_delay = user_arg_value("shot-delay");
	shot_delay_ = shot_delay.empty() ? 0.0 : std::clamp(std::atof(shot_delay.c_str()), 0.0, 600.0);
}

void Bf3LevelSession::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	timings_.ready_us = now_us();
	timings_.process_start_to_ready_s = seconds_since_process_creation();
	parse_args();
	headless_ = DisplayServer::get_singleton()->get_name() == String("headless");
	// The headless skip (docs §11.1): no scene work under the dummy renderer or for --m1-check.
	scene_ = !headless_ && !m1_check_;

	bf3::core::InstallLocateOptions opts;
	opts.cmdline_path = user_arg_value("bf3-install");
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps->has_setting("openbf3/install/path")) {
		opts.setting_path = to_std(String(ps->get_setting("openbf3/install/path")));
	}
	install_.locate(opts);
	if (install_.is_mounted()) {
		install_before_ = install_.snapshot();
	}
	guard_ = install_.write_guard();
	UtilityFunctions::print(to_godot("[m1] engine " + engine_summary()));
	UtilityFunctions::print(to_godot("[m1] install " + install_.describe()));
	const std::u8string root8 = cache_root_.u8string();
	UtilityFunctions::print(to_godot(std::string("[m1] ") + level_ + " " + mode_ + (scene_ ? ", scene on" : ", scene off (headless skip, docs 11.1)") +
			", netsim " + netsim_ + ", walkers " + std::to_string(walkers_) + ", cache " + (no_cache_ ? std::string("off") : std::string(root8.begin(), root8.end()))));

	// The motor tuning (docs §6.11): res://config/motor_tuning.cfg, read here (FileAccess is main-thread work).
	motor_tuning_source_ = "res://config/motor_tuning.cfg";
	if (FileAccess::file_exists(to_godot(motor_tuning_source_))) {
		motor_tuning_text_ = to_std(FileAccess::get_file_as_string(to_godot(motor_tuning_source_)));
	} else {
		motor_tuning_source_ = "(missing: built-in defaults)";
	}
	UtilityFunctions::print(to_godot("[m1] motor tuning " + motor_tuning_source_));
	if (bots_ > 0) { // S1
		UtilityFunctions::print(to_godot("[s1] bots " + std::to_string(bots_) + (nav_overlay_ ? ", nav overlay on (F7)" : "")));
	}

	mapper_.install_actions();
	cameras_.attach(this);
	if (scene_ || !headless_) {
		loading_.attach(this, "openBF3  " + level_ + "  " + mode_);
	}
	build_hud();
	if (scene_) {
		capsule_.instantiate();
		capsule_->set_radius(0.3f);
		capsule_->set_height(1.8f);
		set_loading_display(true);
	}
	if (m1_check_) {
		Engine::get_singleton()->set_max_fps(144); // headless frames at a 144 Hz display's rate (E2 records per frame)
	}
	{
		std::vector<ShotRunner::FreeShot> free_shots;
		std::vector<ShotRunner::SpawnShot> spawn_shots;
		std::string error;
		if (!ShotRunner::parse(user_arg_value("shots"), user_arg_value("shot-spawns"), free_shots, spawn_shots, error)) {
			UtilityFunctions::push_error(to_godot("[m1] " + error));
			failed_ = true;
		}
		shots_ = std::make_unique<ShotRunner>(*this, std::move(free_shots), std::move(spawn_shots), user_arg_value("shot-dir"));
	}
	if (!install_.is_mounted()) {
		std::lock_guard lock(loader_mutex_);
		load_error_ = "no usable install: " + install_.describe();
		load_failed_.store(true, std::memory_order_release);
	} else {
		upload_task_ = callable_mp(this, &Bf3LevelSession::_upload_task);
		lanes_ = std::make_unique<render::SceneLanes>();
		upload_.set_lanes(lanes_.get());
		loader_ = std::thread([this]() { run_loader(); });
	}
	set_process(true);
	set_process_unhandled_input(true);
}

// ---- loader thread: no Godot API ------------------------------------------------------------------------------

void Bf3LevelSession::run_loader() {
	const auto fail = [&](const std::string &p_error) {
		std::lock_guard lock(loader_mutex_);
		load_error_ = p_error;
		progress_.begin(host::LoadStage::Failed);
		load_failed_.store(true, std::memory_order_release);
	};
	std::shared_ptr<bf3::core::Cache> cache;
	if (!no_cache_) {
		const uint64_t t0 = now_us();
		AcquiredCache acquired = acquire_cache(cache_root_, guard_);
		timings_.seconds_cache_open = double(now_us() - t0) / 1e6;
		const std::u8string root8 = cache_root_.u8string();
		const std::string root(root8.begin(), root8.end());
		std::lock_guard lock(loader_mutex_);
		if (acquired.status == bf3::core::CacheStatus::Ok) {
			cache = acquired.cache;
			const bf3::core::CacheStats st = cache->stats();
			char line[512];
			std::snprintf(line, sizeof(line), "cache %s: %s in %.1f ms, %llu entries, %.1f MiB", root.c_str(), acquired.shared ? "shared" : "opened",
					acquired.open_seconds * 1000.0, (unsigned long long)st.entries, mib(st.bytes));
			cache_line_ = line;
		} else {
			cache_line_ = "cache " + root + " not used (" + bf3::core::cache_status_name(acquired.status) + (acquired.detail.empty() ? "" : ": ") +
					acquired.detail + ")";
		}
	} else {
		std::lock_guard lock(loader_mutex_);
		cache_line_ = "cache disabled (--no-cache)";
	}
	bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(install_);
	if (!install) {
		fail(install.error().message);
		return;
	}
	const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
	// docs §12.2: 16 cores = 8 scene lanes + 8 shape threads; on fewer cores half each.
	const unsigned half = std::max(1u, cores / 2);
	bf3::core::Result<host::LevelDatabase> db = host::open_level_database(*install, level_, mode_, 0, &progress_);
	if (!db) {
		fail(db.error().message);
		return;
	}
	if (stop_.load(std::memory_order_acquire)) {
		return;
	}
	if (scene_) {
		progress_.begin(host::LoadStage::Meshes);
		uint64_t t0 = now_us();
		bf3::core::Result<lvl::MeshCensus> census = lvl::mesh_census(*db->level, *db->world);
		if (!census) {
			fail("census: " + census.error().message);
			return;
		}
		timings_.seconds_census = double(now_us() - t0) / 1e6;
		t0 = now_us();
		bf3::core::Result<render::LevelScenePlan> plan = render::plan_level_scene(*db->level, *db->world, *census);
		if (!plan) {
			fail("scene plan: " + plan.error().message);
			return;
		}
		timings_.seconds_plan = double(now_us() - t0) / 1e6;
		auto shared_plan = std::make_shared<const render::LevelScenePlan>(std::move(*plan));
		{
			std::lock_guard lock(loader_mutex_);
			plan_ = shared_plan;
		}
		timings_.plan_us = now_us();
		plan_ready_.store(true, std::memory_order_release);
		render::SceneLaneOptions lane_options;
		lane_options.lanes = std::min(8u, half);
		lane_options.texture_lanes = std::min(6u, std::max(1u, lane_options.lanes - std::min(2u, lane_options.lanes / 4)));
		const bf3::core::Status started = lanes_->start(*shared_plan, db->level->mount(), cache.get(), lane_options, upload_);
		if (!started.ok()) {
			fail("scene lanes: " + started.error().message);
			return;
		}
	}
	host::SharedLoadOptions shared_options;
	shared_options.cache = cache.get();
	shared_options.threads = scene_ ? std::min(8u, half) : 0;
	if (!motor_tuning_text_.empty()) {
		bf3::core::Result<sim::MotorTuning> tuning = sim::parse_motor_tuning(motor_tuning_text_);
		if (!tuning) {
			fail(motor_tuning_source_ + ": " + tuning.error().message);
			return;
		}
		shared_options.tuning = *tuning;
	}
	// S1 (docs/design/s1.md §12): with --bots the static world carries the terrain cover and the water rule of the
	// level's combat-area tiles (every MP mode), the same the navmesh is baked from.
	std::optional<lvl::LevelCombatTiles> s1_combat;
	if (bots_ > 0) {
		bf3::core::Result<lvl::LevelCombatTiles> combat = lvl::level_combat_tiles(*install, level_, sim::NavParams().region_margin);
		if (!combat) {
			fail("combat areas: " + combat.error().message);
			return;
		}
		s1_combat = std::move(*combat);
		shared_options.terrain_cover = s1_combat->tiles;
		shared_options.cover_water = s1_combat->water;
	}
	bf3::core::Result<std::shared_ptr<const host::SharedLevel>> shared = host::load_shared_level(*db, shared_options, &progress_);
	if (!shared) {
		fail(shared.error().message);
		return;
	}
	{
		std::lock_guard lock(loader_mutex_);
		shared_ = *shared;
	}
	timings_.shared_us = now_us();
	shared_ready_.store(true, std::memory_order_release);
	if (stop_.load(std::memory_order_acquire)) {
		return;
	}
	// S1: the bot level (navmesh through the cache, components, goals, bot spawns) and the overlay's arrays.
	if (bots_ > 0) {
		host::BotLoadOptions bot_options;
		bot_options.cache = cache.get();
		bot_options.threads = scene_ ? std::min(8u, half) : 0;
		bot_options.combat = &*s1_combat;
		bot_options.seed = seed_;
		bot_options.synthetic_spawns = bots_;
		const uint64_t t0 = now_us();
		bf3::core::Result<std::shared_ptr<const host::BotLevel>> bots = host::load_bot_level(*db, **shared, bot_options, &progress_);
		if (!bots) {
			fail("bots: " + bots.error().message);
			return;
		}
		bot_level_ = *bots;
		const sim::NavStats &ns = bot_level_->nav->stats();
		char line[512];
		std::snprintf(line, sizeof(line), "[s1] bot level %.2f s: nav %zu polys (%s), main component %u, %zu goals, spawns %u of %u in main%s",
				double(now_us() - t0) / 1e6, ns.polys, ns.world_hit ? "cache hit" : "baked", bot_level_->main_component, bot_level_->objectives.size(),
				bot_level_->spawns.in_main, bot_level_->spawns.level_points, bot_level_->spawns.synthetic > 0 ? " (synthetic spawns)" : "");
		bots_line_ = line;
		if (nav_overlay_ && scene_) {
			nav_arrays_ = prepare_nav_overlay(sim::nav_debug_mesh(*bot_level_->nav, &bot_level_->components), bot_level_->main_component);
			nav_ready_.store(true, std::memory_order_release);
		}
	}
	// Phase D: the server and the client, while the lanes still run (docs §12.1).
	std::string error;
	if (!start_session(&error)) {
		fail(error);
		return;
	}
	timings_.session_us = now_us();
	session_ready_.store(true, std::memory_order_release);
	if (scene_) {
		bf3::core::Result<render::SceneLaneStats> stats = lanes_->wait();
		// Every timing the loader writes (cache open, census, plan, lanes) is written before lanes_finished()
		// publishes lanes_done_ (release); the main thread reads them only after LevelUpload::finished() saw it
		// (acquire).
		timings_.lanes_us = now_us();
		upload_.lanes_finished();
		if (!stats) {
			fail("scene lanes: " + stats.error().message);
			return;
		}
		std::lock_guard lock(loader_mutex_);
		lane_stats_ = *stats;
	}
	// Phase B is over: the lanes no longer read the mount, so the level database goes (docs §12.1).
	db = host::LevelDatabase();
	cache.reset(); // the last holder closes the cache here, on the loader thread
	loader_done_.store(true, std::memory_order_release);
}

host::ClientSessionConfig Bf3LevelSession::client_config(const std::string &p_netsim, uint64_t p_seed) const {
	host::ClientSessionConfig cc;
	cc.port = server_ != nullptr ? server_->port() : 0;
	cc.name = "player";
	bf3::core::Result<bf3::net::NetSimConfig> ns = bf3::net::parse_netsim(p_netsim);
	if (ns) {
		cc.netsim = *ns;
	}
	cc.netsim.seed = p_seed * 7919u + 1u;
	cc.request_dev_commands = true;
	cc.debug_hash_compare = m1_check_;
	return cc;
}

bool Bf3LevelSession::start_session(std::string *r_error) {
	progress_.begin(host::LoadStage::Connect, 1);
	bf3::core::Result<bf3::net::NetSimConfig> ns = bf3::net::parse_netsim(netsim_);
	if (!ns) {
		*r_error = "--netsim=" + netsim_ + ": " + ns.error().message;
		return false;
	}
	auto game = std::make_shared<host::GameServerConfig>();
	game->level = shared_;
	game->max_slots = m1_check_ ? uint32_t(bf3::net::kMaxSlots) : 64u; // one SpawnSweep batch covers Metro's 102 points (docs §7.7)
	game->walkers = walkers_;
	game->dev_commands = true; // M1 is a local listen session: the checks and the shot harness use SpawnAt / Teleport / Trace
	game->seed = seed_;
	if (bot_level_ != nullptr) { // S1 (docs/design/s1.md §12): the walkers, every slot spawning in the main component
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = bot_level_;
		fill->count = bots_;
		fill->spawn_in_main = true;
		game->bots = fill;
		game->max_slots = uint32_t(bf3::net::kMaxSlots);
	}
	host::ServerConfig sc;
	sc.tick_hz = int(sim::kSimHz);
	sc.game = game;
	server_ = std::make_unique<host::ServerHost>();
	std::string err;
	if (!server_->start(sc, &err)) {
		*r_error = "server: " + err;
		return false;
	}
	client_ = std::make_unique<host::ClientSession>();
	const bf3::core::Status s = client_->start(client_config(netsim_, seed_), shared_);
	if (!s.ok()) {
		*r_error = "client: " + s.error().message;
		return false;
	}
	progress_.advance(host::LoadStage::Connect);
	progress_.end(host::LoadStage::Connect);
	progress_.begin(host::LoadStage::Spawn, 1);
	return true;
}

bool Bf3LevelSession::restart_client(const std::string &p_netsim, uint64_t p_seed, std::string *r_error) {
	if (server_ == nullptr || shared_level() == nullptr) {
		*r_error = "no session";
		return false;
	}
	if (!bf3::net::parse_netsim(p_netsim)) {
		*r_error = "bad netsim profile " + p_netsim;
		return false;
	}
	if (client_ != nullptr) {
		client_->stop();
	}
	client_ = std::make_unique<host::ClientSession>();
	*frame_ = host::PresentationFrame();
	mapper_.reset_life();
	mapper_.release_all();
	const bf3::core::Status s = client_->start(client_config(p_netsim, p_seed), shared_level());
	if (!s.ok()) {
		*r_error = s.error().message;
		return false;
	}
	return true;
}

void Bf3LevelSession::_upload_task() {
	upload_.run_task();
}

// ---- main thread -----------------------------------------------------------------------------------------------

std::shared_ptr<const host::SharedLevel> Bf3LevelSession::shared_level() const {
	std::lock_guard lock(loader_mutex_);
	return shared_;
}

std::shared_ptr<const render::LevelScenePlan> Bf3LevelSession::scene_plan() const {
	std::lock_guard lock(loader_mutex_);
	return plan_;
}

std::optional<render::SceneLaneStats> Bf3LevelSession::lane_stats() const {
	std::lock_guard lock(loader_mutex_);
	return lane_stats_;
}

std::string Bf3LevelSession::cache_summary() const {
	std::lock_guard lock(loader_mutex_);
	return cache_line_;
}

void Bf3LevelSession::set_loading_display(bool p_loading) {
	if (headless_) {
		return;
	}
	DisplayServer *ds = DisplayServer::get_singleton();
	Engine *engine = Engine::get_singleton();
	if (p_loading && !loading_display_) {
		saved_vsync_ = int(ds->window_get_vsync_mode());
		saved_max_fps_ = int(engine->get_max_fps());
		ds->window_set_vsync_mode(DisplayServer::VSYNC_DISABLED);
		engine->set_max_fps(0);
		loading_display_ = true;
	} else if (!p_loading && loading_display_) {
		if (saved_vsync_ >= 0) {
			ds->window_set_vsync_mode(DisplayServer::VSyncMode(saved_vsync_));
		}
		engine->set_max_fps(saved_max_fps_);
		loading_display_ = false;
	}
}

void Bf3LevelSession::on_plan_ready() {
	std::shared_ptr<const render::LevelScenePlan> plan = scene_plan();
	LevelUpload::Options options;
	upload_.begin(plan, get_world_3d()->get_scenario(), &progress_, options);
	build_environment(plan->environment);
	char line[512];
	std::snprintf(line, sizeof(line),
			"[m1] scene plan: %zu requests, %zu meshes, %zu instances, %zu materials, %zu textures, %zu terrains, %zu failures (census %.3f s, plan %.3f s); "
			"environment from data %d",
			plan->requests.size(), plan->meshes.size(), plan->instances.size(), plan->materials.size(), plan->textures.size(), plan->terrains.size(),
			plan->failures.size(), timings_.seconds_census, timings_.seconds_plan, int(plan->environment.from_data));
	UtilityFunctions::print(line);
	const render::SceneEnvironment &env = plan->environment;
	std::snprintf(line, sizeof(line),
			"[m1] environment: sun dir (%.3f %.3f %.3f) colour (%.3f %.3f %.3f) energy %.2f; sky (%.3f %.3f %.3f) ground (%.3f %.3f %.3f) ambient %.2f; fog "
			"%.0f..%.0f m (%.3f %.3f %.3f)",
			env.sun_direction[0], env.sun_direction[1], env.sun_direction[2], env.sun_color[0], env.sun_color[1], env.sun_color[2], env.sun_energy, env.sky_color[0],
			env.sky_color[1], env.sky_color[2], env.ground_color[0], env.ground_color[1], env.ground_color[2], env.ambient_energy, env.fog_start, env.fog_end,
			env.fog_color[0], env.fog_color[1], env.fog_color[2]);
	UtilityFunctions::print(line);
	for (const auto &[item, error] : plan->failures) {
		UtilityFunctions::print(to_godot("[m1] plan failure: " + item + ": " + error));
	}
}

void Bf3LevelSession::build_environment(const render::SceneEnvironment &p_env) {
	// docs §11.4: one DirectionalLight3D with shadows and one WorldEnvironment (ProceduralSky from the sky and ground
	// colours, ambient from the sky, depth fog when given, ACES).
	sun_ = memnew(DirectionalLight3D);
	sun_->set_name("Sun");
	Vector3 d(p_env.sun_direction[0], p_env.sun_direction[1], p_env.sun_direction[2]);
	if (d.length_squared() < 1e-6f) {
		d = Vector3(-0.35f, -0.8f, 0.45f);
	}
	d = d.normalized();
	const Vector3 up = std::fabs(d.y) > 0.99f ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
	sun_->set_transform(Transform3D(Basis::looking_at(d, up), Vector3()));
	sun_->set_color(to_color(p_env.sun_color));
	sun_->set_param(Light3D::PARAM_ENERGY, p_env.sun_energy);
	sun_->set_shadow(true);
	sun_->set_param(Light3D::PARAM_SHADOW_MAX_DISTANCE, 250.0f);
	add_child(sun_);

	Ref<ProceduralSkyMaterial> sky_material;
	sky_material.instantiate();
	sky_material->set_sky_top_color(to_color(p_env.sky_color));
	sky_material->set_sky_horizon_color(to_color(p_env.sky_color).lerp(Color(1, 1, 1), 0.35f));
	sky_material->set_ground_bottom_color(to_color(p_env.ground_color));
	sky_material->set_ground_horizon_color(to_color(p_env.ground_color).lerp(to_color(p_env.sky_color), 0.5f));
	Ref<Sky> sky;
	sky.instantiate();
	sky->set_material(sky_material);
	Ref<Environment> env;
	env.instantiate();
	env->set_background(Environment::BG_SKY);
	env->set_sky(sky);
	env->set_ambient_source(Environment::AMBIENT_SOURCE_SKY);
	env->set_ambient_light_energy(p_env.ambient_energy);
	env->set_tonemapper(Environment::TONE_MAPPER_ACES);
	// Fog is opt-in (--fog) [inferred]: the park's FogColor as level_scene reads it is a saturated blue (0.05, 0.38, 1.0)
	// that floods Godot's depth fog over the whole frame; reading BF3's fog properly is the look track's work.
	if (p_env.fog_end > p_env.fog_start && p_env.fog_end > 0.0f && has_user_arg("fog")) {
		env->set_fog_enabled(true);
		env->set_fog_mode(Environment::FOG_MODE_DEPTH);
		env->set_fog_depth_begin(p_env.fog_start);
		env->set_fog_depth_end(p_env.fog_end);
		env->set_fog_light_color(to_color(p_env.fog_color));
	}
	environment_ = memnew(WorldEnvironment);
	environment_->set_name("Environment");
	environment_->set_environment(env);
	add_child(environment_);
}

void Bf3LevelSession::build_hud() {
	if (headless_ || !hud_enabled_) {
		return;
	}
	CanvasLayer *layer = memnew(CanvasLayer);
	layer->set_name("Hud");
	hud_ = memnew(Label);
	hud_->set_position(Vector2(10, 6));
	hud_->add_theme_color_override("font_color", Color(1, 1, 1));
	hud_->add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.9f));
	hud_->add_theme_constant_override("shadow_offset_x", 1);
	hud_->add_theme_constant_override("shadow_offset_y", 1);
	hud_->add_theme_font_size_override("font_size", 14);
	hud_->set_visible(false);
	layer->add_child(hud_);
	add_child(layer);
}

String Bf3LevelSession::get_status_text() const {
	const host::PresentationFrame &f = *frame_;
	const host::ClientStats &s = f.stats;
	const float speed = std::sqrt(f.own_curr.velocity[0] * f.own_curr.velocity[0] + f.own_curr.velocity[2] * f.own_curr.velocity[2]);
	static const char *poses[] = { "stand", "crouch", "prone" };
	char buf[1024];
	std::snprintf(buf, sizeof(buf),
			"openBF3 M1 | %s %s | load %.2f s (process start %.2f s) | %.0f fps\n"
			"tick %u | feet %.2f %.2f %.2f | %.2f m/s | %s%s | eye %.2f m | health %.0f\n"
			"RTT %.1f ms | buffer %d (target %u) | reconciliations %llu | others %u%s\n"
			"WASD move, Shift sprint, Space jump, Ctrl crouch, Z prone, F6 free camera, Esc mouse",
			level_.c_str(), mode_.c_str(), timings_.done_from_ready_s, timings_.done_from_process_s, Engine::get_singleton()->get_frames_per_second(), f.tick,
			f.own_curr.position[0], f.own_curr.position[1], f.own_curr.position[2], speed, poses[std::min<size_t>(size_t(f.own_curr.pose), 2)],
			f.own_curr.transition_ticks > 0 ? " (transition)" : "", double(f.eye_height_curr), double(f.own_curr.health), s.rtt_ms, s.command_buffer_ticks,
			s.target_buffer_ticks, (unsigned long long)s.reconciliations, f.other_count, mapper_.free_camera() ? " | FREE CAMERA" : "");
	return String(buf);
}

void Bf3LevelSession::update_hud(double p_delta) {
	if (hud_ == nullptr) {
		return;
	}
	hud_->set_visible(done_);
	hud_timer_ += p_delta;
	if (done_ && hud_timer_ >= 0.1) {
		hud_timer_ = 0.0;
		hud_->set_text(get_status_text());
		if (bots_ > 0) { // S1: the snapshot carries the nearest others; the rest are not drawn
			const uint32_t total = bots_ + walkers_;
			const uint32_t shown = std::min(frame_->other_count, total);
			hud_->set_text(get_status_text() + to_godot(" | S1 bots " + std::to_string(bots_) + ", others in the snapshot " + std::to_string(shown) + " of " +
					std::to_string(total) + (nav_draw_.uploaded() ? (nav_draw_.visible() ? ", nav overlay (F7)" : ", nav overlay hidden (F7)") : "")));
		}
	}
}

void Bf3LevelSession::draw_others(uint64_t p_now_ns) {
	const host::PresentationFrame &f = *frame_;
	if (!f.valid || f.other_count == 0) {
		for (const RID &r : other_instances_) {
			RenderingServer::get_singleton()->instance_set_visible(r, false);
		}
		return;
	}
	std::array<host::PresentedSoldier, bf3::net::kMaxSlots> out{};
	uint64_t extrapolated = 0;
	const size_t n = host::present_others(f, p_now_ns, out, &extrapolated);
	others_presented_ += n;
	others_extrapolated_ += extrapolated;
	if (!scene_) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	while (other_instances_.size() < n) {
		other_instances_.push_back(rs->instance_create2(capsule_->get_rid(), get_world_3d()->get_scenario()));
	}
	for (size_t i = 0; i < other_instances_.size(); ++i) {
		if (i >= n) {
			rs->instance_set_visible(other_instances_[i], false);
			continue;
		}
		const host::PresentedSoldier &p = out[i];
		// A capsule from the feet to the eye + 0.15 m (scaled to the presented eye height).
		const float height = std::max(0.6f, p.eye_height + 0.15f);
		const Basis basis = Basis(Vector3(0, 1, 0), p.yaw).scaled(Vector3(1.0f, height / 1.8f, 1.0f));
		rs->instance_set_transform(other_instances_[i], Transform3D(basis, Vector3(p.position[0], p.position[1] + height * 0.5f, p.position[2])));
		rs->instance_set_visible(other_instances_[i], true);
	}
}

void Bf3LevelSession::try_done() {
	if (done_ || !session_ready_.load(std::memory_order_acquire)) {
		return;
	}
	const host::PresentationFrame &f = *frame_;
	const bool spawned = f.valid && f.stats.spawned && f.own_curr.life > 0 && mapper_.adopted_life() == f.own_curr.life;
	if (spawned && progress_.end_us[size_t(host::LoadStage::Spawn)].load(std::memory_order_relaxed) == 0) {
		progress_.advance(host::LoadStage::Spawn);
		progress_.end(host::LoadStage::Spawn);
	}
	if (scene_ && upload_.begun() && upload_.finished() && timings_.upload_us == 0) {
		timings_.upload_us = now_us();
		progress_.end(host::LoadStage::Meshes);
		progress_.end(host::LoadStage::Textures);
		progress_.end(host::LoadStage::Upload);
	}
	if (!spawned || (scene_ && timings_.upload_us == 0)) {
		return;
	}
	done_ = true;
	timings_.done_us = now_us();
	timings_.done_from_process_s = seconds_since_process_creation();
	timings_.done_from_ready_s = double(timings_.done_us - timings_.ready_us) / 1e6;
	progress_.begin(host::LoadStage::Done);
	progress_.end(host::LoadStage::Done);
	loading_.remove();
	set_loading_display(false);
	if (!mapper_.scripted() && !headless_) {
		mapper_.set_capture(true);
	}
	const auto stage_s = [&](host::LoadStage p_stage) {
		const uint64_t a = progress_.start_us[size_t(p_stage)].load(), b = progress_.end_us[size_t(p_stage)].load();
		return a != 0 && b >= a ? double(b - a) / 1e6 : 0.0;
	};
	char line[1024];
	std::snprintf(line, sizeof(line),
			"[m1] done: process start -> first controllable frame %.3f s, _ready -> done %.3f s (process start -> _ready %.3f s); stages mount %.3f, db %.3f, "
			"meshes %.3f, textures %.3f, shapes %.3f, upload %.3f, connect %.3f, spawn %.3f s; cache %s; peak working set %.1f MiB",
			timings_.done_from_process_s, timings_.done_from_ready_s, timings_.process_start_to_ready_s, stage_s(host::LoadStage::Mount),
			stage_s(host::LoadStage::Database), stage_s(host::LoadStage::Meshes), stage_s(host::LoadStage::Textures), stage_s(host::LoadStage::Shapes),
			stage_s(host::LoadStage::Upload), stage_s(host::LoadStage::Connect), stage_s(host::LoadStage::Spawn),
			cache_warmth_name(host::CacheWarmth(progress_.warmth.load())), mib(peak_working_set_bytes()));
	UtilityFunctions::print(line);
	UtilityFunctions::print(to_godot("[m1] " + cache_summary()));
	if (!bots_line_.empty()) { // S1
		UtilityFunctions::print(to_godot(bots_line_));
	}
	if (m1_check_) {
		uint32_t courses = 6;
		if (has_user_arg("m1-courses")) {
			courses = uint32_t(std::clamp(std::atoi(user_arg_value("m1-courses").c_str()), 1, 102));
		}
		check_ = std::make_unique<M1Check>(*this, courses);
	} else if (!load_check_.empty()) {
		load_check_runner_ = std::make_unique<M1LoadCheck>(*this, load_check_ == "warm", shots_.get());
	}
}

void Bf3LevelSession::_unhandled_input(const Ref<InputEvent> &p_event) {
	if (!done_) {
		return;
	}
	mapper_.handle_event(p_event);
	// S1: F7 toggles the nav overlay.
	if (const InputEventKey *k = Object::cast_to<InputEventKey>(p_event.ptr())) {
		if (k->is_pressed() && !k->is_echo() && k->get_keycode() == KEY_F7 && nav_draw_.uploaded()) {
			nav_draw_.set_visible(!nav_draw_.visible());
		}
	}
	if (const InputEventMouseButton *mb = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {
		if (mb->is_pressed() && mapper_.free_camera()) {
			if (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP) {
				cameras_.scale_free_speed(1.25f);
			} else if (mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN) {
				cameras_.scale_free_speed(0.8f);
			}
		}
	}
}

void Bf3LevelSession::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || quitting_) {
		return;
	}
	++frames_;
	const uint64_t now_ns = bf3::net::monotonic_ns();
	if (load_failed_.load(std::memory_order_acquire) && !failed_) {
		failed_ = true;
		std::string error;
		{
			std::lock_guard lock(loader_mutex_);
			error = load_error_;
		}
		UtilityFunctions::push_error(to_godot("[m1] load failed: " + error));
		UtilityFunctions::print(to_godot("[m1] load failed: " + error));
		if (m1_check_ || !load_check_.empty() || quit_after_ > 0.0) {
			finish(1);
			return;
		}
	}
	if (scene_ && plan_ready_.load(std::memory_order_acquire) && !upload_.begun()) {
		on_plan_ready();
	}
	if (scene_ && upload_.begun() && !failed_) {
		upload_.pump(upload_task_, done_ ? 2.0 : 12.0);
	}
	if (session_ready_.load(std::memory_order_acquire) && client_ != nullptr) {
		client_->read_presentation(*frame_);
		mapper_.adopt(*frame_);
		mapper_.poll_devices();
		client_->set_input(mapper_.sample());
	}
	try_done();
	if (!done_) {
		++timings_.loading_frames;
		timings_.loading_frame_ms_sum += p_delta * 1000.0;
		timings_.loading_frame_ms_max = std::max(timings_.loading_frame_ms_max, p_delta * 1000.0);
		std::string detail;
		if (upload_.begun()) {
			const UploadStats st = upload_.stats();
			char line[256];
			std::snprintf(line, sizeof(line), "upload: %llu meshes, %llu textures, %llu instances, %llu materials (C1 %llu tasks)",
					(unsigned long long)(st.meshes + st.terrains), (unsigned long long)st.textures, (unsigned long long)st.instances,
					(unsigned long long)st.materials, (unsigned long long)st.tasks);
			detail = line;
		}
		std::string error;
		if (failed_) {
			std::lock_guard lock(loader_mutex_);
			error = "load failed: " + load_error_;
		}
		loading_.update(progress_, now_us(), error.empty() ? detail : error);
		return;
	}
	since_done_ += p_delta;
	frame_ms_after_done_.push_back(float(p_delta * 1000.0));
	if (frame_ms_after_done_.size() > 100000) {
		frame_ms_after_done_.erase(frame_ms_after_done_.begin(), frame_ms_after_done_.begin() + 50000);
	}
	if (cameras_.free() != mapper_.free_camera()) {
		cameras_.set_free(mapper_.free_camera());
	}
	const ViewAngles &view = mapper_.view();
	(void)cameras_.update_first_person(*frame_, now_ns, view.yaw, view.pitch);
	if (cameras_.free()) {
		const ViewAngles &fv = mapper_.free_view();
		cameras_.update_free(p_delta, mapper_.free_move(), mapper_.free_speed_factor(), fv.yaw, fv.pitch);
	}
	draw_others(now_ns);
	update_hud(p_delta);
	s1_upload_overlay(); // S1
	s1_report_others(now_ns); // S1

	if (check_ != nullptr) {
		if (check_->frame(now_ns)) {
			finish(check_->exit_code());
		}
		return;
	}
	if (load_check_runner_ != nullptr) {
		if (load_check_runner_->frame(now_ns, p_delta)) {
			finish(load_check_runner_->exit_code());
		}
		return;
	}
	// S1: --shot-delay holds the shots (and the quit after them) until the bots have spread out.
	if (shot_delay_ > 0.0 && since_done_ < shot_delay_ && shots_ != nullptr && !shots_->empty()) {
		return;
	}
	bool shots_done = true;
	if (shots_ != nullptr && !shots_->empty()) {
		shots_done = shots_->frame(now_ns);
		if (shots_done && quit_after_ <= 0.0) {
			finish(shots_->failed() == 0 && !failed_ ? 0 : 1);
			return;
		}
	}
	if (quit_after_ > 0.0 && since_done_ >= quit_after_ && shots_done) {
		finish(failed_ || (shots_ != nullptr && shots_->failed() > 0) ? 1 : 0);
	}
}

bool Bf3LevelSession::save_screenshot(const std::string &p_path) {
	const std::filesystem::path target = utf8_path(p_path);
	if (!guard_.allows_write(target)) {
		UtilityFunctions::push_error(to_godot("[m1] screenshot refused (" + std::string(bf3::core::PathGuard::verdict_name(guard_.check_write_target(target))) +
				"): " + p_path));
		return false;
	}
	if (headless_) {
		return false;
	}
	if (target.has_parent_path()) {
		std::error_code ec;
		std::filesystem::create_directories(target.parent_path(), ec); // under the same guard verdict as the file
	}
	Ref<Image> img = get_viewport()->get_texture()->get_image();
	if (img.is_null() || img->save_png(to_godot(p_path)) != OK) {
		UtilityFunctions::push_error(to_godot("[m1] screenshot failed: " + p_path));
		return false;
	}
	return true;
}

void Bf3LevelSession::finish(int p_code) {
	if (quitting_) {
		return;
	}
	quitting_ = true;
	if (install_before_.valid) {
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(install_before_, install_.snapshot());
		UtilityFunctions::print(to_godot(std::string("[m1] install unchanged: ") + (d.clean() ? "yes" : "NO: " + d.summary())));
		if (!d.clean()) {
			p_code = 1;
		}
	}
	UtilityFunctions::print(to_godot("[m1] peak working set " + std::to_string(int(mib(peak_working_set_bytes()) + 0.5)) + " MiB, exit " + std::to_string(p_code)));
	shutdown();
	get_tree()->quit(p_code);
}

} // namespace bf3::godot_side

// ---- S1 (docs/design/s1.md §12) ---------------------------------------------------------------------------------

namespace bf3::godot_side {

void Bf3LevelSession::s1_upload_overlay() {
	if (!scene_ || nav_draw_.uploaded() || !nav_ready_.load(std::memory_order_acquire)) {
		return;
	}
	if (nav_draw_.upload(nav_arrays_, get_world_3d()->get_scenario(), true)) {
		char line[256];
		std::snprintf(line, sizeof(line), "[s1] nav overlay: %zu main-component triangles, %zu other, %zu drop links", nav_arrays_.triangles_main,
				nav_arrays_.triangles_other, nav_arrays_.links);
		UtilityFunctions::print(line);
	}
	nav_arrays_ = NavOverlayArrays(); // the RenderingServer holds the copy
}

// Once, when the shots start (or 10 s after Done): how many others are presented, how far they walked from their
// spawns is not known here, so their positions' spread and a few positions (evidence for the shots, docs §12).
void Bf3LevelSession::s1_report_others(uint64_t p_now_ns) {
	if (bots_ == 0 || s1_reported_ || since_done_ < std::max(shot_delay_, 10.0) || !frame_->valid) {
		return;
	}
	s1_reported_ = true;
	std::array<host::PresentedSoldier, bf3::net::kMaxSlots> out{};
	const size_t n = host::present_others(*frame_, p_now_ns, out, nullptr);
	float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	size_t moving = 0;
	for (size_t i = 0; i < n; ++i) {
		for (int k = 0; k < 3; ++k) {
			lo[k] = std::min(lo[k], out[i].position[size_t(k)]);
			hi[k] = std::max(hi[k], out[i].position[size_t(k)]);
		}
		moving += std::hypot(out[i].velocity[0], out[i].velocity[2]) > 0.5f ? 1 : 0;
	}
	char line[768];
	int w = std::snprintf(line, sizeof(line), "[s1] %.1f s after done: %zu others presented, %zu moving (> 0.5 m/s), feet within (%.0f %.0f %.0f)..(%.0f %.0f %.0f);",
			since_done_, n, moving, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
	for (size_t i = 0; i < std::min<size_t>(n, 8) && w > 0 && size_t(w) < sizeof(line); ++i) {
		w += std::snprintf(line + w, sizeof(line) - size_t(w), " (%.1f %.1f %.1f)", out[i].position[0], out[i].position[1], out[i].position[2]);
	}
	UtilityFunctions::print(line);
}

} // namespace bf3::godot_side
