// Bf3Crowd: the S3 crowd node and its --s3-check (docs/design/s3.md §1, §3, §7, §9, §12). See s3_crowd.h.
#include "s3_crowd.h"

#include "godot_cache.h"
#include "godot_util.h"
#include "level_session.h"
#include "render_bridge.h"

#include "bf3/core/install_snapshot.h"
#include "bf3/sim/collision_world.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdarg>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <intrin.h>
#include <windows.h>

namespace bf3::godot_side {

using namespace godot;
namespace anim = bf3::anim;
namespace gate = bf3::anim::s3_gate;

namespace {

constexpr double kShotDistance = 0.9; // s3.md §9
constexpr float kShotFov = 40.0f;
constexpr int kShotSettleFrames = 4; // frames between posing and capturing (the pose must have been drawn)
constexpr uint64_t kMinShotBytes = 100 * 1024;

void log(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[s3] " + p_line));
}

std::filesystem::path utf8_path(const std::string &p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

std::string path_text(const std::filesystem::path &p_path) {
	const std::u8string u = p_path.u8string();
	return std::string(u.begin(), u.end());
}

double percentile(std::vector<double> p_values, double p_q) {
	if (p_values.empty()) {
		return 0.0;
	}
	std::sort(p_values.begin(), p_values.end());
	const double at = p_q * double(p_values.size() - 1);
	const size_t i = size_t(at);
	const double f = at - double(i);
	return i + 1 < p_values.size() ? p_values[i] * (1.0 - f) + p_values[i + 1] * f : p_values.back();
}

Vector3 gv(const anim::Vec3 &p_v) {
	return Vector3(p_v.x, p_v.y, p_v.z);
}

bf3::sim::Vec3 sv(const anim::Vec3 &p_v) {
	return bf3::sim::Vec3{ p_v.x, p_v.y, p_v.z };
}

const char *vsync_name(DisplayServer::VSyncMode p_mode) {
	switch (p_mode) {
		case DisplayServer::VSYNC_DISABLED:
			return "disabled";
		case DisplayServer::VSYNC_ENABLED:
			return "enabled";
		case DisplayServer::VSYNC_ADAPTIVE:
			return "adaptive";
		case DisplayServer::VSYNC_MAILBOX:
			return "mailbox";
	}
	return "?";
}

uint64_t thread_cycles() {
	ULONG64 c = 0;
	QueryThreadCycleTime(GetCurrentThread(), &c);
	return uint64_t(c);
}

std::string fmt(const char *p_format, ...) {
	char buf[1024];
	va_list args;
	va_start(args, p_format);
	std::vsnprintf(buf, sizeof(buf), p_format, args);
	va_end(args);
	return buf;
}

const char *pass_text(bool p_ok) {
	return p_ok ? "PASS" : "FAIL";
}

} // namespace

Bf3Crowd::Bf3Crowd() = default;

Bf3Crowd::~Bf3Crowd() {
	shutdown();
}

void Bf3Crowd::_bind_methods() {}

void Bf3Crowd::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		shutdown();
	}
}

void Bf3Crowd::parse_args() {
	check_ = has_user_arg("s3-check");
	diag_ = has_user_arg("s3-diag");
	walkers_zero_ = has_user_arg("walkers") && user_arg_value("walkers") == "0";
	no_hud_ = has_user_arg("no-hud");
	no_cache_ = has_user_arg("no-cache");
	const std::string backend = user_arg_value("s3-backend");
	backend_ = backend == "skeleton3d" ? SkinBackend::Skeleton3D : SkinBackend::RsSkeleton; // texture: not built (s3.md §7.3)
	lod_ = user_arg_value("s3-lod") != "off";
	if (has_user_arg("s3-seconds")) {
		run_seconds_ = std::atof(user_arg_value("s3-seconds").c_str());
	}
	if (check_ || !(run_seconds_ >= gate::kMinSeconds) || !std::isfinite(run_seconds_)) {
		run_seconds_ = std::max(gate::kMinSeconds, std::isfinite(run_seconds_) ? std::min(run_seconds_, 600.0) : gate::kMinSeconds);
	}
	shot_dir_ = user_arg_value("s3-shot-dir");
	if (shot_dir_.empty()) {
		// <repo>/b/shots/s3: the project folder's parent is the checkout.
		const std::filesystem::path project = utf8_path(to_std(ProjectSettings::get_singleton()->globalize_path("res://")));
		shot_dir_ = path_text((project / "..").lexically_normal() / "b" / "shots" / "s3");
	}
	if (!no_cache_) {
		const std::string root = user_arg_value("bf3-cache");
		cache_root_ = root.empty() ? default_cache_root() : utf8_path(root);
	}
}

void Bf3Crowd::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	parse_args();
	session_ = Object::cast_to<Bf3LevelSession>(get_node_or_null(NodePath("../Session")));
	if (session_ == nullptr) {
		UtilityFunctions::push_error("[s3] Bf3Crowd needs a sibling Bf3LevelSession named Session");
		return;
	}
	team_of_.resize(stage_.soldiers());
	for (uint32_t i = 0; i < stage_.soldiers(); ++i) {
		team_of_[i] = (i % stage_.cols) < stage_.cols / 2 ? 0 : 1;
	}
	log(std::string("crowd: ") + (check_ ? "--s3-check" : "interactive") + ", backend " + skin_backend_name(backend_) + ", temporal LOD " + (lod_ ? "on" : "off") +
			", run " + std::to_string(int(run_seconds_)) + " s, shots " + shot_dir_);
	if (check_) {
		DisplayServer *ds = DisplayServer::get_singleton();
		const int32_t primary = ds->get_primary_screen();
		const Vector2i size = ds->screen_get_size(primary);
		if (size.x != int32_t(gate::kWidth) || size.y != int32_t(gate::kHeight) || ds->get_name() == String("headless")) {
			log(fmt("SKIPPED: the primary screen is %dx%d (%s), the gate is defined at %ux%u", size.x, size.y, to_std(ds->get_name()).c_str(), gate::kWidth,
					gate::kHeight));
			skip_ = true;
		} else {
			ds->window_set_mode(DisplayServer::WINDOW_MODE_WINDOWED);
			ds->window_set_flag(DisplayServer::WINDOW_FLAG_BORDERLESS, true);
			ds->window_set_current_screen(primary);
			ds->window_set_position(ds->screen_get_position(primary));
			ds->window_set_size(Vector2i(int32_t(gate::kWidth), int32_t(gate::kHeight)));
		}
	}
	set_process(true);
}

void Bf3Crowd::start_loader() {
	started_ = true;
	const bf3::core::InstallReader &install = session_->install();
	const bf3::core::PathGuard guard = install.write_guard();
	loader_ = std::thread([this, &install, guard]() {
		std::shared_ptr<bf3::core::Cache> cache;
		if (!cache_root_.empty()) {
			AcquiredCache acquired = acquire_cache(cache_root_, guard);
			if (acquired.status == bf3::core::CacheStatus::Ok) {
				cache = acquired.cache;
				cache_line_ = std::string("cache ") + path_text(cache_root_) + (acquired.shared ? " shared" : " opened");
			} else {
				cache_line_ = "cache " + path_text(cache_root_) + " not used (" + bf3::core::cache_status_name(acquired.status) + ")";
			}
		} else {
			cache_line_ = "cache disabled (--no-cache)";
		}
		bf3::core::Result<std::unique_ptr<bf3::render::SoldierAssets>> r = bf3::render::load_soldier_assets(install, bf3::render::SoldierSpec{}, &progress_,
				cache.get());
		cache_ = std::move(cache);
		if (r) {
			loaded_ = std::move(*r);
		} else {
			load_error_ = r.error().message;
		}
		assets_ready_.store(true, std::memory_order_release);
	});
}

bool Bf3Crowd::validate_stage(std::string *r_error) {
	const std::shared_ptr<const bf3::host::SharedLevel> level = session_->shared_level();
	if (!level || !level->static_world) {
		*r_error = "no static world";
		return false;
	}
	bf3::core::Result<std::unique_ptr<bf3::sim::CollisionWorld>> world = bf3::sim::CollisionWorld::create(level->static_world);
	if (!world) {
		*r_error = "CollisionWorld: " + world.error().message;
		return false;
	}
	world_ = std::move(*world);
	const bf3::sim::CollisionWorld &cw = *world_;
	const uint32_t los = bf3::sim::query_mask::kCollision | bf3::sim::query_mask::kRaycast;
	const anim::StageCamera cam = anim::stage_camera(stage_);
	const bf3::sim::Vec3 c = sv(cam.position);
	if (cw.overlap_capsule(bf3::sim::Vec3{ c[0], c[1] - 0.3f, c[2] }, 0.25f, 0.6f, bf3::sim::query_mask::kCollision, 0.0f)) {
		*r_error = "the camera point is inside geometry";
		return false;
	}
	const float rx = std::cos(stage_.heading), rz = -std::sin(stage_.heading);
	feet_.assign(stage_.soldiers(), anim::Vec3{});
	for (uint32_t i = 0; i < stage_.soldiers(); ++i) {
		anim::Vec3 p = anim::stage_cell(stage_, i);
		bf3::sim::StaticHit hit;
		if (!cw.ground_below(bf3::sim::Vec3{ p.x, p.y + 1.0f, p.z }, 2.0f, bf3::sim::query_mask::kCollision, &hit) ||
				std::fabs(hit.position[1] - stage_.origin.y) > 0.3f) {
			*r_error = "cell " + std::to_string(i) + ": no floor within 0.3 m";
			return false;
		}
		p.y = hit.position[1];
		feet_[i] = p;
		if (cw.overlap_capsule(bf3::sim::Vec3{ p.x, p.y + 0.05f, p.z }, 0.45f, 1.85f, bf3::sim::query_mask::kCollision, 0.02f)) {
			*r_error = "cell " + std::to_string(i) + ": the soldier capsule overlaps geometry";
			return false;
		}
		for (const float h : { 0.2f, 1.0f, 1.8f }) {
			for (const float side : { -0.4f, 0.4f }) {
				const bf3::sim::Vec3 t{ p.x + rx * side, p.y + h, p.z + rz * side };
				if (cw.ray(c, bf3::sim::Vec3{ t[0] - c[0], t[1] - c[1], t[2] - c[2] }, los)) {
					*r_error = "cell " + std::to_string(i) + fmt(": the camera ray to %.1f m high, %+.1f m sideways is occluded", h, side);
					return false;
				}
			}
		}
	}
	return true;
}

bool Bf3Crowd::make_evaluator(bool p_lod, std::string *r_error) {
	if (eval_ && lanes_) {
		eval_->wait_and_publish(*lanes_);
	}
	config_ = anim::CrowdConfig{};
	config_.temporal_lod = p_lod;
	config_.lod = assets_->lod;
	bf3::core::Result<anim::AimMeasure> aim = anim::measure_aim(assets_->clips, config_.aim_order);
	if (aim && aim->qualifies()) {
		config_.aim_up_full_pitch_deg = aim->up_deg;
		config_.aim_down_full_pitch_deg = -aim->down_deg;
	}
	bf3::core::Result<std::unique_ptr<anim::CrowdEvaluator>> e = anim::CrowdEvaluator::make(assets_->clips, teams_, stage_.soldiers(), config_);
	if (!e) {
		*r_error = e.error().message;
		return false;
	}
	eval_ = std::move(*e);
	return true;
}

bool Bf3Crowd::make_uploader(SkinBackend p_backend, std::string *r_error) {
	if (uploader_ && uploader_->backend() == p_backend) {
		uploader_->set_visible(true);
		return true;
	}
	if (uploader_) {
		uploader_->clear();
		uploader_.reset();
	}
	std::unique_ptr<SkinUploader> u = make_skin_uploader(p_backend, this);
	if (!u) {
		*r_error = std::string("backend ") + skin_backend_name(p_backend) + " is not built";
		return false;
	}
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	if (!u->build(*assets_, *eval_, team_of_, get_world_3d()->get_scenario(), r_error)) {
		u->clear();
		return false;
	}
	log(fmt("uploader %s built in %.1f ms", skin_backend_name(p_backend), double(Time::get_singleton()->get_ticks_usec() - t0) / 1000.0));
	uploader_ = std::move(u);
	backend_ = p_backend;
	return true;
}

bool Bf3Crowd::build(std::string *r_error) {
	for (const std::string &n : assets_->notes) {
		log(n);
	}
	log(cache_line_);
	log(fmt("assets loaded in %.2f s (ant %.2f, ebx %.2f, meshes %.2f, textures %.2f)", assets_->seconds_total, assets_->seconds_ant, assets_->seconds_ebx,
			assets_->seconds_meshes, assets_->seconds_textures));
	{
		// The shot folder: never inside the install (PathGuard), created when missing.
		const std::filesystem::path dir = utf8_path(shot_dir_);
		std::error_code ec;
		shot_dir_ok_ = session_->install().write_guard().allows_write(dir / "s3_overview.png");
		if (shot_dir_ok_) {
			std::filesystem::create_directories(dir, ec);
			shot_dir_ok_ = !ec;
		}
		log(std::string("shot folder ") + shot_dir_ + (shot_dir_ok_ ? "" : " REFUSED (inside the install or not creatable): no shots"));
	}
	g0_stage_ = validate_stage(&stage_error_);
	log(std::string("G0-stage: ") + (g0_stage_ ? "all 64 cells pass" : "FAIL: " + stage_error_));
	if (feet_.empty()) {
		feet_.assign(stage_.soldiers(), anim::Vec3{});
		for (uint32_t i = 0; i < stage_.soldiers(); ++i) {
			feet_[i] = anim::stage_cell(stage_, i);
		}
	}
	teams_.assign(2, anim::CrowdTeam{});
	for (size_t t = 0; t < 2; ++t) {
		const bf3::render::SoldierTeam &team = assets_->teams[t];
		teams_[t].binds = &team.binds;
		teams_[t].reload = &team.reload;
		teams_[t].weapon_joints = &team.weapon_joints;
		teams_[t].weapon_local = team.weapon_local;
	}
	const unsigned hc = std::thread::hardware_concurrency();
	const unsigned lanes = std::clamp(hc == 0 ? 2u : (hc > 4 ? hc - 4 : 0u), 2u, 6u);
	lanes_ = std::make_unique<bf3::core::WorkerGroup>(lanes, "s3 pose");
	log(fmt("pose lanes: %u (hardware_concurrency %u)", lanes, hc));
	if (!make_evaluator(check_ ? false : lod_, r_error)) {
		return false;
	}
	log(fmt("aim: weight 1 at %.1f deg up, %.1f deg down; additive order %s", config_.aim_up_full_pitch_deg, config_.aim_down_full_pitch_deg,
			config_.aim_order == anim::AdditiveOrder::LocalPost ? "LocalPost" : "LocalPre"));
	if (!make_uploader(check_ ? SkinBackend::Skeleton3D : backend_, r_error)) {
		return false;
	}
	inputs_.resize(stage_.soldiers());
	roots_.resize(stage_.soldiers());
	camera_ = memnew(Camera3D);
	camera_->set_name("CrowdCamera");
	camera_->set_keep_aspect_mode(Camera3D::KEEP_HEIGHT);
	camera_->set_near(0.05f);
	camera_->set_far(2000.0f);
	add_child(camera_);
	const anim::StageCamera sc = anim::stage_camera(stage_);
	camera_->set_fov(sc.fov_y_deg);
	camera_->look_at_from_position(gv(sc.position), gv(sc.target), Vector3(0, 1, 0));
	camera_->make_current();
	RenderingServer::get_singleton()->viewport_set_measure_render_time(get_viewport()->get_viewport_rid(), true);
	built_ = true;
	return true;
}

void Bf3Crowd::step(double p_delta) {
	const double prev_t = stage_time_;
	stage_time_ += std::clamp(p_delta, 0.0, 0.25);
	const anim::Vec3 cam = anim::stage_camera(stage_).position;
	for (uint32_t i = 0; i < stage_.soldiers(); ++i) {
		anim::CrowdSoldierInput in = anim::stage_input(stage_, i, stage_time_, prev_t, cam, lod_spread_);
		in.position = feet_[i];
		inputs_[i] = in;
		roots_[i] = Transform3D(Basis(Vector3(0, 1, 0), in.yaw), gv(in.position));
	}
	eval_->wait_and_publish(*lanes_);
	uploader_->upload(*eval_, roots_, upload_stats_);
	eval_->set_inputs(inputs_, p_delta);
	eval_->kick(*lanes_);
}

uint32_t Bf3Crowd::count_in_view() const {
	if (camera_ == nullptr) {
		return 0;
	}
	uint32_t n = 0;
	for (uint32_t i = 0; i < stage_.soldiers(); ++i) {
		const Vector3 f = gv(feet_[i]);
		bool in = true;
		for (int k = 0; k < 8 && in; ++k) {
			const Vector3 p = f + Vector3((k & 1) ? stage_.box_half : -stage_.box_half, (k & 2) ? stage_.box_height : 0.0f, (k & 4) ? stage_.box_half : -stage_.box_half);
			in = camera_->is_position_in_frustum(p);
		}
		n += in ? 1 : 0;
	}
	return n;
}

void Bf3Crowd::calibrate_cycles() {
	LARGE_INTEGER f, a, b;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&a);
	const uint64_t t0 = __rdtsc();
	do {
		QueryPerformanceCounter(&b);
	} while (double(b.QuadPart - a.QuadPart) / double(f.QuadPart) < 0.25);
	const uint64_t t1 = __rdtsc();
	const double ms = double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart);
	cycles_per_ms_ = double(t1 - t0) / ms;
	// Self-test: thread cycles count at the TSC rate. Best of 20 busy windows of 5 ms (a preempted window reads fewer
	// thread cycles than TSC cycles; the best window is the unpreempted one).
	double best = 0.0;
	for (int k = 0; k < 20; ++k) {
		const uint64_t c0 = thread_cycles(), r0 = __rdtsc();
		uint64_t r1 = r0;
		while (double(r1 - r0) < cycles_per_ms_ * 5.0) {
			r1 = __rdtsc();
		}
		const uint64_t c1 = thread_cycles();
		const double ratio = double(c1 - c0) / double(r1 - r0);
		if (std::fabs(ratio - 1.0) < std::fabs(best - 1.0)) {
			best = ratio;
		}
	}
	cycle_selftest_ratio_ = best;
	log(fmt("cycle timer: %.0f TSC cycles per ms (QueryPerformanceCounter over %.0f ms); self-test thread/TSC cycles %.4f (best of 20 x 5 ms)", cycles_per_ms_, ms,
			best));
}

double Bf3Crowd::thread_ms() const {
	return cycles_per_ms_ > 0.0 ? double(thread_cycles()) / cycles_per_ms_ : 0.0;
}

void Bf3Crowd::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || session_ == nullptr || shut_down_) {
		return;
	}
	if (skip_) {
		skip_ = false;
		finish(77);
		return;
	}
	if (finished_) {
		return;
	}
	if (!session_->done()) {
		return;
	}
	if (!started_) {
		start_loader();
		return;
	}
	if (!built_) {
		if (!assets_ready_.load(std::memory_order_acquire)) {
			return;
		}
		loader_.join();
		if (!loaded_) {
			log("load failed: " + load_error_);
			UtilityFunctions::push_error(to_godot("[s3] load failed: " + load_error_));
			finish(1);
			return;
		}
		assets_ = std::move(loaded_);
		std::string error;
		if (!build(&error)) {
			log("build failed: " + error);
			finish(1);
			return;
		}
		if (check_) {
			calibrate_cycles();
			settle_frames_ = 30; // let the window settle at its size before the configuration row is read
		}
		last_entry_usec_ = Time::get_singleton()->get_ticks_usec();
		last_entry_cpu_ms_ = thread_ms();
		return;
	}
	// Per-frame timers (s3.md §1): the _process delta, the main-thread CPU since the previous entry.
	const uint64_t entry = Time::get_singleton()->get_ticks_usec();
	const double entry_cpu = thread_ms();
	Sample s;
	s.frame_ms = float(double(entry - last_entry_usec_) / 1000.0);
	s.other_cpu_ms = float(std::max(0.0, entry_cpu - last_entry_cpu_ms_ - prev_step_cpu_ms_));
	s.prev_animated = prev_animated_;
	last_entry_usec_ = entry;
	last_entry_cpu_ms_ = entry_cpu;
	const RID vp = get_viewport()->get_viewport_rid();
	s.render_cpu_ms = float(RenderingServer::get_singleton()->viewport_get_measured_render_time_cpu(vp));
	s.render_gpu_ms = float(RenderingServer::get_singleton()->viewport_get_measured_render_time_gpu(vp));

	bool animated = !frozen_;
	if (check_ && settle_frames_ > 0) {
		animated = true;
	}
	if (check_ && shots_started_) {
		animated = false; // the shots pose the soldiers themselves
	}
	if (animated) {
		const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
		const double c0 = thread_ms();
		step(p_delta);
		s.step_cpu_ms = float(thread_ms() - c0);
		s.step_ms = float(double(Time::get_singleton()->get_ticks_usec() - t0) / 1000.0);
		const anim::CrowdFrameStats &st = eval_->stats();
		s.evaluated = st.evaluated;
		s.calls = upload_stats_.calls;
		s.job_ms = float(st.job_seconds * 1000.0);
		s.blocked_ms = float(st.blocked_seconds * 1000.0);
	}
	prev_step_cpu_ms_ = animated ? s.step_cpu_ms : 0.0;
	prev_animated_ = animated;
	s.animated = animated;
	if (!check_) {
		return;
	}
	if (settle_frames_ > 0) {
		if (--settle_frames_ == 0) {
			// G0-config (s3.md §1).
			DisplayServer *ds = DisplayServer::get_singleton();
			// The root Window's size is the 3D render size (stretch mode canvas_items keeps the 2D visible rect at the
			// project's 1152x648 base; 3D renders at the window's full resolution).
			Window *window = get_window();
			const Vector2i size = window != nullptr ? window->get_size() : Vector2i();
			viewport_ok_ = size.x == int32_t(gate::kWidth) && size.y == int32_t(gate::kHeight) &&
					ds->window_get_current_screen() == ds->get_primary_screen();
			const int thread_model = int(ProjectSettings::get_singleton()->get_setting("rendering/driver/threads/thread_model", 1));
			const bool cycles_ok = std::fabs(cycle_selftest_ratio_ - 1.0) <= 0.05;
			g0_config_ = walkers_zero_ && no_hud_ && viewport_ok_ && thread_model == 1 && cycles_ok;
			g0_config_detail_ = fmt("walkers=0 %s, no-hud %s, window %dx%d on screen %d (primary %d) %s, thread_model %d (1 = Safe), cycle self-test %.4f %s",
					walkers_zero_ ? "yes" : "NO", no_hud_ ? "yes" : "NO", size.x, size.y, ds->window_get_current_screen(), ds->get_primary_screen(),
					viewport_ok_ ? "ok" : "WRONG", thread_model, cycle_selftest_ratio_, cycles_ok ? "ok" : "OUT OF 1 +- 0.05");
			log("G0-config: " + g0_config_detail_);
			// The run plan (s3.md §12).
			plan_.clear();
			if (g0_stage_ && g0_config_) {
				plan_.push_back(RunSpec{ "bench skeleton3d", SkinBackend::Skeleton3D, false, false, false, gate::kBenchWindows, false });
				plan_.push_back(RunSpec{ "bench rs", SkinBackend::RsSkeleton, false, false, false, gate::kBenchWindows, false });
				plan_.push_back(RunSpec{ "R1", SkinBackend::RsSkeleton, false, false, false, 0, true });
				plan_.push_back(RunSpec{ "R2", SkinBackend::RsSkeleton, false, false, false, gate::kR2Windows, false });
				plan_.push_back(RunSpec{ "R3 lod=on spread", SkinBackend::RsSkeleton, true, true, false, 0, false });
				plan_.push_back(RunSpec{ "R4 vsync=on", SkinBackend::RsSkeleton, false, false, true, 0, false });
				if (diag_) {
					plan_.push_back(RunSpec{ "Rdiag roots-only", SkinBackend::RsSkeleton, false, false, false, gate::kBenchWindows * 2, false, false });
				}
			}
			plan_index_ = 0;
			if (plan_.empty()) {
				report_and_finish();
				return;
			}
			begin_run(plan_[0]);
		}
		return;
	}
	if (run_) {
		check_frame(p_delta, s, animated);
		return;
	}
	if (shots_started_) {
		if (shot_frame()) {
			report_and_finish();
		}
	}
}

void Bf3Crowd::begin_run(const RunSpec &p_spec) {
	RunSpec spec = p_spec;
	if (spec.label.rfind("R", 0) == 0) {
		spec.backend = winner_;
		spec.label += std::string(" ") + skin_backend_name(winner_);
	}
	std::string error;
	if (!make_uploader(spec.backend, &error) || !make_evaluator(spec.lod, &error)) {
		log("run " + spec.label + " setup failed: " + error);
		failed_ = true;
		run_.reset();
		plan_index_ = plan_.size();
		report_and_finish();
		return;
	}
	lod_spread_ = spec.lod_spread;
	uploader_->set_palette_upload(spec.palettes);
	DisplayServer::get_singleton()->window_set_vsync_mode(spec.vsync ? DisplayServer::VSYNC_ENABLED : DisplayServer::VSYNC_DISABLED);
	Engine::get_singleton()->set_max_fps(0);
	frozen_ = false;
	run_ = std::make_unique<RunState>();
	run_->spec = spec;
	log("run " + spec.label + fmt(": %s, temporal LOD %s%s, vsync %s, %s", skin_backend_name(spec.backend), spec.lod ? "on" : "off", spec.lod_spread ? " (tier spread)" : "",
			spec.vsync ? "on" : "off", spec.windows > 0 ? (std::to_string(spec.windows) + " windows of 5 s").c_str() : (std::to_string(int(run_seconds_)) + " s").c_str()));
}

void Bf3Crowd::check_frame(double p_delta, const Sample &p_sample, bool p_animated) {
	RunState &r = *run_;
	r.t += p_delta;
	++r.frames;
	const double warm = gate::kWarmupSeconds;
	const bool windowed = r.spec.windows > 0;
	const double total = windowed ? warm + double(r.spec.windows) * gate::kWindowSeconds : warm + run_seconds_;
	// The frame that ends: samples belong to the run once warm-up is over; window settle time is discarded.
	if (r.t > warm) {
		bool keep = true;
		if (windowed) {
			const double into = std::fmod(r.t - warm, gate::kWindowSeconds);
			keep = into >= gate::kWindowSettleSeconds;
		}
		if (keep) {
			Sample s = p_sample;
			s.animated = p_animated;
			r.samples.push_back(s);
			r.measured_seconds += p_delta;
			if (p_animated) {
				const anim::CrowdFrameStats &st = eval_->stats();
				r.blend_evaluated += st.evaluated;
				r.blend_two += st.two_cycle;
				r.blend_idle += st.idle_mix;
				r.blend_clips += st.loco_clips_sampled;
				for (size_t k = 0; k < r.tiers.size(); ++k) {
					r.tiers[k] += st.evaluated_by_tier[k];
				}
			}
			if (r.frames % 30 == 0) {
				r.in_view_min = std::min(r.in_view_min, count_in_view());
				other_zero_ = other_zero_ && session_->frame().other_count == 0;
				const DisplayServer::VSyncMode vm = DisplayServer::get_singleton()->window_get_vsync_mode();
				r.vsync_read = vsync_name(vm);
				const bool want = r.spec.vsync;
				vsync_ok_ = vsync_ok_ && (want ? vm == DisplayServer::VSYNC_ENABLED : vm == DisplayServer::VSYNC_DISABLED) && Engine::get_singleton()->get_max_fps() == 0;
			}
		}
		if (r.spec.overview && !r.overview_done) {
			r.overview_done = true;
			const std::string path = path_text(utf8_path(shot_dir_) / "s3_overview.png");
			const bool ok = shot_dir_ok_ && session_->save_screenshot(path);
			log(std::string("overview shot ") + path + (ok ? "" : " FAILED"));
		}
	}
	// Window class of the next frame.
	if (windowed && r.t > warm) {
		const uint32_t w = uint32_t((r.t - warm) / gate::kWindowSeconds);
		frozen_ = (w % 2) == 1;
	} else {
		frozen_ = false;
	}
	if (r.t < total) {
		return;
	}
	frozen_ = false;
	// The run is over: summarize.
	if (windowed) {
		const HiddenCostStats h = hidden_cost(r);
		log(fmt("%s: animated step p50 %.3f p99 %.3f ms (cpu p50 %.3f); hidden p50 %.3f ms (other cpu animated %.3f, frozen %.3f); fps %.0f / %.0f; render cpu "
				"%.2f / %.2f ms; calls p50 %u",
				r.spec.label.c_str(), h.animated.step_p50_ms, h.animated.step_p99_ms, h.animated.step_cpu_p50_ms, h.hidden_p50_ms, h.animated.other_cpu_p50_ms,
				h.frozen.other_cpu_p50_ms, h.animated.fps, h.frozen.fps, h.animated.render_cpu_p50_ms, h.frozen.render_cpu_p50_ms, h.animated.calls_p50));
		if (r.spec.label.rfind("bench", 0) == 0) {
			const double metric = h.animated.step_p99_ms + std::max(0.0, h.hidden_p50_ms);
			bench_metric_[r.spec.backend == SkinBackend::Skeleton3D ? 0 : 1] = metric;
			log(fmt("%s: selection metric p99(step) + max(0, hidden) = %.3f ms", r.spec.label.c_str(), metric));
		}
	} else {
		const CrowdRunStats st = summarize(r, -1);
		log(fmt("%s: %u frames %.1f s, fps %.0f; step p50 %.3f p99 %.3f ms (cpu p50 %.3f); frame p50 %.2f p99 %.2f ms; render cpu %.2f gpu %.2f ms; job p50 %.3f "
				"p99 %.3f ms, blocked p99 %.3f ms; evaluated %.1f/frame; calls p50 %u; in view min %u; vsync %s",
				r.spec.label.c_str(), st.frames, st.seconds, st.fps, st.step_p50_ms, st.step_p99_ms, st.step_cpu_p50_ms, st.frame_p50_ms, st.frame_p99_ms,
				st.render_cpu_p50_ms, st.render_gpu_p50_ms, st.job_p50_ms, st.job_p99_ms, st.blocked_p99_ms, st.evaluated_mean, st.calls_p50, st.in_view_min,
				st.vsync_mode.c_str()));
		if (r.spec.lod) {
			std::string tiers;
			for (size_t k = 0; k < r.tiers.size(); ++k) {
				tiers += fmt(" %.1f", st.evaluated_by_tier_per_s[k] / double(stage_.cols));
			}
			log(r.spec.label + ": evaluations per second per soldier by tier (<First, First..Sixth, >far):" + tiers);
		}
	}
	const bool was_bench_b = r.spec.label.rfind("bench rs", 0) == 0;
	done_runs_.push_back(std::move(r));
	run_.reset();
	if (was_bench_b) {
		// s3.md §7.3: the lower metric wins; within 10 % B wins.
		const double a = bench_metric_[0], b = bench_metric_[1];
		winner_ = (a >= 0.0 && b >= 0.0 && a < b * 0.9) ? SkinBackend::Skeleton3D : SkinBackend::RsSkeleton;
		log(fmt("backend selection: skeleton3d %.3f ms, rs %.3f ms -> %s", a, b, skin_backend_name(winner_)));
	}
	if (++plan_index_ < plan_.size()) {
		begin_run(plan_[plan_index_]);
		return;
	}
	// Shots (G3).
	DisplayServer::get_singleton()->window_set_vsync_mode(DisplayServer::VSYNC_DISABLED);
	shots_.clear();
	for (uint32_t team = 0; team < 2; ++team) {
		for (int pose = 0; pose < 4; ++pose) {
			for (const bool lf : { true, false }) {
				shots_.push_back(ShotSpec{ team, pose, lf });
			}
		}
	}
	shot_index_ = 0;
	shot_wait_ = -1;
	shots_started_ = true;
	uploader_->set_palette_upload(true);
	if (eval_ && lanes_) {
		eval_->wait_and_publish(*lanes_);
	}
	for (uint32_t i = 0; i < stage_.soldiers(); ++i) {
		uploader_->set_soldier_visible(i, i == 3 || i == 4);
	}
}

CrowdRunStats Bf3Crowd::summarize(const RunState &p_run, int p_class) const {
	CrowdRunStats st;
	st.label = p_run.spec.label;
	st.vsync_mode = p_run.vsync_read;
	std::vector<double> step, step_cpu, other, frame, rcpu, rgpu, job, blocked, calls;
	double evaluated = 0.0, seconds = 0.0;
	for (const Sample &s : p_run.samples) {
		const bool step_class = p_class < 0 || (p_class == 1) == s.animated;
		const bool other_class = p_class < 0 || (p_class == 1) == (s.prev_animated && s.animated);
		if (other_class && (p_class < 0 || s.prev_animated == s.animated)) {
			other.push_back(s.other_cpu_ms);
		}
		if (!step_class) {
			continue;
		}
		++st.frames;
		seconds += s.frame_ms / 1000.0;
		frame.push_back(s.frame_ms);
		rcpu.push_back(s.render_cpu_ms);
		rgpu.push_back(s.render_gpu_ms);
		if (s.animated) {
			step.push_back(s.step_ms);
			step_cpu.push_back(s.step_cpu_ms);
			job.push_back(s.job_ms);
			blocked.push_back(s.blocked_ms);
			calls.push_back(double(s.calls));
			evaluated += s.evaluated;
		}
	}
	st.seconds = seconds;
	st.fps = seconds > 0.0 ? double(st.frames) / seconds : 0.0;
	st.step_p50_ms = percentile(step, 0.5);
	st.step_p99_ms = percentile(step, 0.99);
	st.step_cpu_p50_ms = percentile(step_cpu, 0.5);
	st.other_cpu_p50_ms = percentile(other, 0.5);
	st.frame_p50_ms = percentile(frame, 0.5);
	st.frame_p99_ms = percentile(frame, 0.99);
	st.render_cpu_p50_ms = percentile(rcpu, 0.5);
	st.render_gpu_p50_ms = percentile(rgpu, 0.5);
	st.job_p50_ms = percentile(job, 0.5);
	st.job_p99_ms = percentile(job, 0.99);
	st.blocked_p99_ms = percentile(blocked, 0.99);
	st.calls_p50 = uint32_t(percentile(calls, 0.5));
	st.evaluated_mean = step.empty() ? 0.0 : evaluated / double(step.size());
	st.in_view_min = p_run.in_view_min == UINT32_MAX ? 0 : p_run.in_view_min;
	if (p_run.blend_evaluated > 0) {
		st.two_cycle_fraction = double(p_run.blend_two) / double(p_run.blend_evaluated);
		st.idle_mix_fraction = double(p_run.blend_idle) / double(p_run.blend_evaluated);
		st.mean_loco_clips = double(p_run.blend_clips) / double(p_run.blend_evaluated);
	}
	const double measured = std::max(1e-6, p_run.measured_seconds);
	for (size_t k = 0; k < st.evaluated_by_tier_per_s.size(); ++k) {
		st.evaluated_by_tier_per_s[k] = double(p_run.tiers[k]) / measured;
	}
	return st;
}

HiddenCostStats Bf3Crowd::hidden_cost(const RunState &p_run) const {
	HiddenCostStats h;
	h.animated = summarize(p_run, 1);
	h.frozen = summarize(p_run, 0);
	h.hidden_p50_ms = h.animated.other_cpu_p50_ms - h.frozen.other_cpu_p50_ms;
	return h;
}

bool Bf3Crowd::shot_frame() {
	if (shot_index_ >= shots_.size()) {
		return true;
	}
	const ShotSpec &shot = shots_[shot_index_];
	const uint32_t soldier = shot.team == 0 ? 3 : 4;
	static const char *const kPose[4] = { "idle", "run", "reload", "aim" };
	const std::string name = std::string("s3_") + (shot.team == 0 ? "us" : "ru") + "_" + kPose[shot.pose] + "_" + (shot.left_front ? "lf" : "rb");
	if (shot_wait_ < 0) {
		// Pose both soldiers on a fresh evaluator (only 3 and 4 evaluated), upload, aim the camera.
		std::string error;
		if (!make_evaluator(false, &error)) {
			shot_lines_.push_back(name + ": evaluator failed: " + error);
			++shot_index_;
			return false;
		}
		const float heading = stage_.heading + 3.14159265f; // facing the stage camera
		std::vector<anim::CrowdSoldierInput> in(stage_.soldiers());
		for (uint32_t i = 0; i < stage_.soldiers(); ++i) {
			in[i].flags = anim::crowd_flags::kHidden;
			in[i].position = feet_[i];
			in[i].yaw = heading;
			in[i].team = team_of_[i];
		}
		for (const uint32_t i : { 3u, 4u }) {
			in[i].flags = 0;
			if (shot.pose == 1) {
				in[i].velocity = anim::Vec3{ std::sin(heading) * stage_.run_speed, 0.0f, std::cos(heading) * stage_.run_speed };
			}
			if (shot.pose == 2) {
				in[i].flags = anim::crowd_flags::kReloadStart;
			}
			if (shot.pose == 3) {
				in[i].pitch = stage_.aim_pitch_deg * 0.0174532925f;
			}
			roots_[i] = Transform3D(Basis(Vector3(0, 1, 0), heading), gv(feet_[i]));
		}
		eval_->set_inputs(in, 0.0);
		eval_->evaluate_all_here();
		for (const uint32_t i : { 3u, 4u }) {
			in[i].flags = 0;
		}
		// Run: RunFwd at phase 0.25; reload: 40 % into the soldier's own clip (s3.md §9). Both soldiers advance by the
		// same dt, so the reload is posed per team (the RU clip is longer): this shot's team decides the dt.
		double dt = 0.0;
		if (shot.pose == 1) {
			const anim::LocoClipInfo &fwd = assets_->clips.loco[size_t(anim::LocoClip::Fwd)];
			const double rate = std::clamp(double(stage_.run_speed) / double(fwd.root_speed), double(config_.loco.min_rate), double(config_.loco.max_rate));
			dt = 0.25 * double(fwd.duration) / rate;
		} else if (shot.pose == 2) {
			dt = 0.4 * double(assets_->teams[shot.team].reload.tracks.duration_seconds());
		}
		eval_->set_inputs(in, dt);
		eval_->evaluate_all_here();
		SkinUploadStats st;
		uploader_->upload(*eval_, roots_, st);
		// The camera: 0.9 m from the shoulder joint, front-left of the left shoulder or back-right of the right one.
		const std::span<const anim::Xform> model = eval_->model(soldier);
		const int32_t joint = assets_->clips.rig.find(shot.left_front ? "LeftArm" : "RightArm");
		if (joint < 0 || model.size() <= size_t(joint)) {
			shot_lines_.push_back(name + ": no shoulder joint");
			++shot_index_;
			return false;
		}
		const Vector3 j = roots_[soldier].xform(gv(model[size_t(joint)].t));
		const Vector3 fwd(std::sin(heading), 0.0f, std::cos(heading));
		const Vector3 left(std::cos(heading), 0.0f, -std::sin(heading)); // +x of the soldier's frame (s3.md §5.8)
		Vector3 dir = shot.left_front ? (fwd + left) : (-fwd - left);
		dir = (dir.normalized() + Vector3(0, 0.25f, 0)).normalized();
		const Vector3 cam = j + dir * float(kShotDistance);
		camera_->set_fov(kShotFov);
		camera_->look_at_from_position(cam, j, Vector3(0, 1, 0));
		shot_wait_ = kShotSettleFrames;
		return false;
	}
	if (--shot_wait_ > 0) {
		return false;
	}
	// Capture with the on-screen and occlusion checks.
	const std::span<const anim::Xform> model = eval_->model(soldier);
	const int32_t joint = assets_->clips.rig.find(shot.left_front ? "LeftArm" : "RightArm");
	const Vector3 j = roots_[soldier].xform(gv(model[size_t(joint)].t));
	const Vector3 cam = camera_->get_global_transform().origin;
	const Vector2 px = camera_->unproject_position(j);
	const Vector2 size = get_viewport()->get_visible_rect().size;
	const bool on_screen = !camera_->is_position_behind(j) && px.x >= 0 && px.y >= 0 && px.x < size.x && px.y < size.y;
	const bf3::sim::Vec3 c{ cam.x, cam.y, cam.z };
	const bool clear_point = world_ && !world_->overlap_capsule(bf3::sim::Vec3{ cam.x, cam.y - 0.3f, cam.z }, 0.25f, 0.6f, bf3::sim::query_mask::kCollision, 0.0f);
	const bool clear_ray = world_ && !world_->ray(c, bf3::sim::Vec3{ j.x - cam.x, j.y - cam.y, j.z - cam.z },
											   bf3::sim::query_mask::kCollision | bf3::sim::query_mask::kRaycast);
	const std::string path = path_text(utf8_path(shot_dir_) / (name + ".png"));
	const bool saved = shot_dir_ok_ && session_->save_screenshot(path);
	std::error_code ec;
	const uint64_t bytes = saved ? uint64_t(std::filesystem::file_size(utf8_path(path), ec)) : 0;
	const bf3::render::PinchReport p =
			bf3::render::measure_pinch(assets_->teams[shot.team], assets_->clips.rig, eval_->model(soldier), eval_->palette(soldier));
	const bool ok = saved && !ec && bytes >= kMinShotBytes && on_screen && clear_point && clear_ray;
	shots_ok_ += ok ? 1 : 0;
	shot_lines_.push_back(fmt("%s: %s, %.0f KB, joint at pixel (%.0f, %.0f)%s%s%s; pinch left %.3f (%u of %u < 0.7), right %.3f (%u of %u < 0.7); hand-IK %.4f m",
			name.c_str(), ok ? "ok" : "FAIL", double(bytes) / 1024.0, px.x, px.y, on_screen ? "" : " OFF SCREEN", clear_point ? "" : " CAMERA BLOCKED",
			clear_ray ? "" : " OCCLUDED", p.min_ratio_left, p.below_left, p.vertices_left, p.min_ratio_right, p.below_right, p.vertices_right, p.hand_ik_m));
	log(shot_lines_.back());
	++shot_index_;
	shot_wait_ = -1;
	return shot_index_ >= shots_.size();
}

void Bf3Crowd::report_and_finish() {
	report_.clear();
	auto row = [&](const std::string &p_id, bool p_ok, const std::string &p_text, bool p_valid = true) {
		report_.push_back(fmt("%-10s %-7s %s", p_id.c_str(), p_valid ? pass_text(p_ok) : "INVALID", p_text.c_str()));
		return p_ok;
	};
	const RunState *r1 = nullptr, *r2 = nullptr, *r3 = nullptr, *r4 = nullptr;
	for (const RunState &r : done_runs_) {
		if (r.spec.label.rfind("R1", 0) == 0) {
			r1 = &r;
		} else if (r.spec.label.rfind("R2", 0) == 0) {
			r2 = &r;
		} else if (r.spec.label.rfind("R3", 0) == 0) {
			r3 = &r;
		} else if (r.spec.label.rfind("R4", 0) == 0) {
			r4 = &r;
		}
	}
	bool valid = g0_stage_ && g0_config_ && r1 != nullptr;
	bool all = true;
	all &= row("G0-stage", g0_stage_, g0_stage_ ? "fixed Metro stage: floor, capsules, camera point and 6 rays per soldier clear" : stage_error_);
	const bool cfg = g0_config_ && other_zero_ && vsync_ok_;
	all &= row("G0-config", cfg, g0_config_detail_ + fmt("; other_count 0 on every sampled frame %s; vsync read-back and max_fps 0 %s", other_zero_ ? "yes" : "NO",
										vsync_ok_ ? "yes" : "NO"));
	valid = valid && cfg;
	CrowdRunStats s1;
	if (r1 != nullptr) {
		s1 = summarize(*r1, -1);
		uint32_t view_min = UINT32_MAX;
		for (const RunState &r : done_runs_) {
			view_min = std::min(view_min, r.in_view_min);
		}
		const bool view = view_min == gate::kInView;
		valid = valid && view;
		all &= row("G0-view", view, fmt("in-view minimum %u of 64 over every run (every 30th frame)", view_min == UINT32_MAX ? 0 : view_min));
		const bool blend = s1.two_cycle_fraction >= gate::kMinTwoCycleFraction && s1.idle_mix_fraction >= gate::kMinIdleMixFraction &&
				s1.mean_loco_clips >= gate::kMinMeanLocoClips;
		valid = valid && blend;
		all &= row("G0-blend", blend, fmt("R1: two-cycle %.1f %% (>= 25), idle mix %.1f %% (>= 1), %.2f locomotion clips per soldier-frame (>= 1.25)",
											   s1.two_cycle_fraction * 100.0, s1.idle_mix_fraction * 100.0, s1.mean_loco_clips));
	} else {
		all &= row("G0-view", false, "not measured");
		all &= row("G0-blend", false, "not measured");
	}
	bool timing_ok = true;
	if (r1 != nullptr) {
		const bool g1a = s1.step_p50_ms <= gate::kMainThreadMs && s1.step_p99_ms <= gate::kMainThreadMs;
		timing_ok &= row("G1a", g1a, fmt("step p50 %.3f ms, p99 %.3f ms (<= 2.0), %s, 64 in view, LOD off, vsync off, %.1f s", s1.step_p50_ms, s1.step_p99_ms,
												 skin_backend_name(winner_), s1.seconds),
				valid);
		if (r2 != nullptr) {
			const HiddenCostStats h = hidden_cost(*r2);
			const double g1b_v = s1.step_p50_ms + std::max(0.0, h.hidden_p50_ms);
			timing_ok &= row("G1b", g1b_v <= gate::kMainThreadMs,
					fmt("p50(G1a) %.3f + max(0, hidden %.3f) = %.3f ms (<= 2.0); R2 step cpu p50 %.3f ms; other cpu animated %.3f / frozen %.3f ms; fps %.0f / "
						"%.0f; render cpu %.2f / %.2f ms; frame p50 %.2f / %.2f, p99 %.2f / %.2f ms (animated / frozen)",
							s1.step_p50_ms, h.hidden_p50_ms, g1b_v, h.animated.step_cpu_p50_ms, h.animated.other_cpu_p50_ms, h.frozen.other_cpu_p50_ms, h.animated.fps,
							h.frozen.fps, h.animated.render_cpu_p50_ms, h.frozen.render_cpu_p50_ms, h.animated.frame_p50_ms, h.frozen.frame_p50_ms,
							h.animated.frame_p99_ms, h.frozen.frame_p99_ms),
					valid);
		} else {
			timing_ok &= row("G1b", false, "not measured", valid);
		}
		const bool g2off = s1.frame_p50_ms <= gate::kFrameMs && s1.frame_p99_ms <= gate::kFrameMs;
		timing_ok &= row("G2-off", g2off, fmt("frame p50 %.2f ms, p99 %.2f ms (<= 16.7), %.0f fps; render cpu %.2f gpu %.2f ms", s1.frame_p50_ms, s1.frame_p99_ms, s1.fps,
												  s1.render_cpu_p50_ms, s1.render_gpu_p50_ms),
				valid);
		if (r4 != nullptr) {
			const CrowdRunStats s4 = summarize(*r4, -1);
			timing_ok &= row("G2-on", s4.frame_p50_ms <= gate::kFrameMs && s4.frame_p99_ms <= gate::kFrameMs,
					fmt("frame p50 %.2f ms, p99 %.2f ms (<= 16.7), %.0f fps, vsync read back %s; step p50 %.3f p99 %.3f ms", s4.frame_p50_ms, s4.frame_p99_ms, s4.fps,
							s4.vsync_mode.c_str(), s4.step_p50_ms, s4.step_p99_ms),
					valid);
		} else {
			timing_ok &= row("G2-on", false, "not measured", valid);
		}
		if (r3 != nullptr) {
			const CrowdRunStats s3 = summarize(*r3, -1);
			std::string tiers;
			for (size_t k = 0; k < s3.evaluated_by_tier_per_s.size(); ++k) {
				tiers += fmt(" %.1f", s3.evaluated_by_tier_per_s[k] / double(stage_.cols));
			}
			report_.push_back(fmt("%-10s %-7s step p50 %.3f p99 %.3f ms (R1 %.3f / %.3f); job p50 %.3f ms (R1 %.3f); evaluated %.1f/frame; per-soldier updates/s by tier:%s",
					"R3", "report", s3.step_p50_ms, s3.step_p99_ms, s1.step_p50_ms, s1.step_p99_ms, s3.job_p50_ms, s1.job_p50_ms, s3.evaluated_mean, tiers.c_str()));
		}
		for (const RunState &r : done_runs_) {
			if (r.spec.label.rfind("Rdiag", 0) == 0) {
				const HiddenCostStats d = hidden_cost(r);
				report_.push_back(fmt("%-10s %-7s roots only (no palettes): hidden %.3f ms (R2 with palettes: see G1b); render cpu %.2f / %.2f ms; fps %.0f / %.0f", "diag",
						"report", d.hidden_p50_ms, d.animated.render_cpu_p50_ms, d.frozen.render_cpu_p50_ms, d.animated.fps, d.frozen.fps));
			}
		}
		report_.push_back(fmt("%-10s %-7s skeleton3d %.3f ms, rs %.3f ms -> %s", "bench", "report", bench_metric_[0], bench_metric_[1], skin_backend_name(winner_)));
		report_.push_back(fmt("%-10s %-7s R1 job p50 %.3f p99 %.3f ms, blocked p99 %.3f ms, %.1f evaluated/frame, %u Godot calls/frame (p50)", "pose", "report",
				s1.job_p50_ms, s1.job_p99_ms, s1.blocked_p99_ms, s1.evaluated_mean, s1.calls_p50));
	} else {
		for (const char *g : { "G1a", "G1b", "G2-off", "G2-on" }) {
			timing_ok &= row(g, false, "not measured", false);
		}
	}
	const bool g3 = shots_ok_ == 16;
	all &= row("G3", g3, fmt("%u of 16 shoulder shots ok in %s (the shoulder judgement is the user's)", shots_ok_, shot_dir_.c_str()));
	for (const std::string &l : shot_lines_) {
		report_.push_back("           " + l);
	}
	const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(session_->install_snapshot_at_start(), session_->install().snapshot());
	all &= row("install", d.clean(), d.clean() ? "unchanged (layout.toc, cas.cat, every .sb/.toc: sizes and mtimes)" : d.summary());
	const bool pass = all && valid && timing_ok;
	log("---- S3 CHECK (shared machine: timings are noisy) ----");
	for (const std::string &l : report_) {
		log(l);
	}
	if (all && valid && !timing_ok) {
		log("S3 TIMING: FAIL"); // the wrapper's one repeat (s3.md §1 noise policy)
	}
	log(std::string("S3 CHECK: ") + (pass ? "PASS" : "FAIL"));
	finish(pass ? 0 : 1);
}

void Bf3Crowd::finish(int p_code) {
	if (finished_) {
		return;
	}
	finished_ = true;
	shutdown();
	session_->finish(p_code);
}

void Bf3Crowd::shutdown() {
	if (shut_down_) {
		return;
	}
	shut_down_ = true;
	if (eval_ && lanes_) {
		eval_->wait_and_publish(*lanes_);
	}
	if (uploader_) {
		uploader_->clear();
		uploader_.reset();
	}
	world_.reset();
	if (loader_.joinable()) {
		loader_.join();
	}
	eval_.reset();
	lanes_.reset();
	cache_.reset();
}

} // namespace bf3::godot_side
