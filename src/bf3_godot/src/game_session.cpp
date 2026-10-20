#include "game_session.h"

#include "godot_util.h"
#include "s0_selftest.h"

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace bf3::godot_side {

using namespace godot;

GameSession::GameSession() = default;

GameSession::~GameSession() {
	stop_server();
}

void GameSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_server"), &GameSession::start_server);
	ClassDB::bind_method(D_METHOD("stop_server"), &GameSession::stop_server);
	ClassDB::bind_method(D_METHOD("is_server_running"), &GameSession::is_server_running);
	ClassDB::bind_method(D_METHOD("get_server_ticks"), &GameSession::get_server_ticks);
	ClassDB::bind_method(D_METHOD("get_status_text"), &GameSession::get_status_text);
	ClassDB::bind_method(D_METHOD("set_autostart", "value"), &GameSession::set_autostart);
	ClassDB::bind_method(D_METHOD("get_autostart"), &GameSession::get_autostart);
	ClassDB::bind_method(D_METHOD("set_dynamic_bodies", "value"), &GameSession::set_dynamic_bodies);
	ClassDB::bind_method(D_METHOD("get_dynamic_bodies"), &GameSession::get_dynamic_bodies);
	ClassDB::bind_method(D_METHOD("set_server_bodies_offset", "value"), &GameSession::set_server_bodies_offset);
	ClassDB::bind_method(D_METHOD("get_server_bodies_offset"), &GameSession::get_server_bodies_offset);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "autostart"), "set_autostart", "get_autostart");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "dynamic_bodies", PROPERTY_HINT_RANGE, "0,256,1"), "set_dynamic_bodies", "get_dynamic_bodies");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "server_bodies_offset"), "set_server_bodies_offset", "get_server_bodies_offset");
}

void GameSession::_notification(int p_what) {

	if (p_what == NOTIFICATION_PREDELETE) {
		stop_server();
	}
}

void GameSession::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}

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
	UtilityFunctions::print(to_godot("[openbf3] engine " + engine_summary()));
	UtilityFunctions::print(to_godot("[openbf3] install " + install_.describe()));

	screenshot_path_ = user_arg_value("screenshot");
	const std::string quit_after = user_arg_value("quit-after");
	quit_after_ = quit_after.empty() ? 0.0 : std::atof(quit_after.c_str());

	if (autostart_ && !start_server()) {
		UtilityFunctions::push_error("[openbf3] ServerHost failed to start");
	}
	build_server_body_nodes();
	build_overlay();
	if (has_user_arg("s0-selftest")) {
		const std::string secs = user_arg_value("selftest-seconds");
		selftest_ = std::make_unique<S0SelfTest>(*this, secs.empty() ? 5.0 : std::atof(secs.c_str()));
		selftest_->begin();
	}
	set_process(true);
	set_physics_process(true);
}

bool GameSession::start_server() {
	if (server_ && server_->running()) {
		return true;
	}
	bf3::host::ServerConfig cfg;
	cfg.world.dynamic_bodies = dynamic_bodies_;
	server_ = std::make_unique<bf3::host::ServerHost>();
	std::string err;
	if (!server_->start(cfg, &err)) {
		UtilityFunctions::push_error(to_godot("[openbf3] ServerHost: " + err));
		server_.reset();
		return false;
	}
	client_ = std::make_unique<bf3::host::LoopbackClient>();
	if (!client_->start(server_->port(), 10, &err)) {
		UtilityFunctions::push_error(to_godot("[openbf3] LoopbackClient: " + err));
	}
	UtilityFunctions::print(to_godot("[openbf3] ServerHost running on 127.0.0.1:" + std::to_string(server_->port()) + ", " +
			std::to_string(cfg.tick_hz) + " Hz, own Jolt " + bf3::sim::PhysicsWorld::jolt_version()));
	return true;
}

void GameSession::stop_server() {
	if (client_) {
		client_->stop();
		client_.reset();
	}
	if (server_) {
		server_->stop();
		server_.reset();
	}
}

bool GameSession::is_server_running() const {
	return server_ && server_->running();
}

int64_t GameSession::get_server_ticks() const {
	return server_ ? static_cast<int64_t>(server_->stats().ticks) : 0;
}

bf3::host::ServerStats GameSession::server_stats() const {
	return server_ ? server_->stats() : bf3::host::ServerStats{};
}

bf3::host::LoopbackClientStats GameSession::client_stats() const {
	return client_ ? client_->stats() : bf3::host::LoopbackClientStats{};
}

String GameSession::get_status_text() const {
	char buf[1024];
	const bf3::host::ServerStats s = server_stats();
	const bf3::host::LoopbackClientStats c = client_stats();
	std::snprintf(buf, sizeof(buf),
			"openBF3 S0 | Godot %s | fps %.0f\n"
			"server: %s tick %llu (%.1f Hz) | step %.3f ms, p99 %.3f ms, max %.3f ms | overruns %llu\n"
			"own Jolt %s: %d bodies, %d awake, lowest y %.2f m, hash %016llx\n"
			"ENet loopback: RTT %.2f ms (mean %.2f, tick-bound), ENet %u ms | pongs %llu/%llu | peers %u, port %u\n"
			"install: %s",
			engine_summary().c_str(), Engine::get_singleton()->get_frames_per_second(), s.running ? "RUNNING" : "stopped",
			static_cast<unsigned long long>(s.ticks), s.measured_hz, s.last_step_ms, s.p99_step_ms, s.max_step_ms,
			static_cast<unsigned long long>(s.overruns), bf3::sim::PhysicsWorld::jolt_version(), s.dynamic_bodies, s.active_bodies,
			s.lowest_body_y, static_cast<unsigned long long>(s.state_hash), c.last_rtt_ms, c.mean_rtt_ms, c.enet_rtt_ms,
			static_cast<unsigned long long>(c.pongs_received), static_cast<unsigned long long>(c.pings_sent), s.peers, s.port,
			install_.describe().c_str());
	return to_godot(buf);
}

void GameSession::build_overlay() {
	overlay_layer_ = memnew(CanvasLayer);
	overlay_layer_->set_name("DebugOverlay");
	overlay_ = memnew(Label);
	overlay_->set_name("Stats");
	overlay_->set_position(Vector2(12, 8));
	overlay_->add_theme_color_override("font_color", Color(1, 1, 1));
	overlay_->add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.9f));
	overlay_->add_theme_constant_override("shadow_offset_x", 1);
	overlay_->add_theme_constant_override("shadow_offset_y", 1);
	overlay_->add_theme_font_size_override("font_size", 15);
	overlay_layer_->add_child(overlay_);
	add_child(overlay_layer_);
	update_overlay();
}

void GameSession::build_server_body_nodes() {
	if (!server_) {
		return;
	}
	server_->copy_body_transforms(body_scratch_);
	Ref<StandardMaterial3D> mat;
	mat.instantiate();
	mat->set_albedo(Color(0.95f, 0.55f, 0.15f));
	for (const bf3::sim::BodyTransform &t : body_scratch_) {
		MeshInstance3D *mi = memnew(MeshInstance3D);
		if (t.shape == 0) {
			Ref<BoxMesh> m;
			m.instantiate();
			m->set_size(Vector3(1, 1, 1) * (2.0f * t.half_extent));
			mi->set_mesh(m);
		} else {
			Ref<SphereMesh> m;
			m.instantiate();
			m->set_radius(t.half_extent);
			m->set_height(2.0f * t.half_extent);
			mi->set_mesh(m);
		}
		mi->set_material_override(mat);
		mi->set_name(to_godot("ServerBody" + std::to_string(body_nodes_.size())));
		add_child(mi);
		body_nodes_.push_back(mi);
	}
	update_server_body_nodes();
}

void GameSession::update_server_body_nodes() {
	if (!server_ || body_nodes_.empty()) {
		return;
	}
	server_->copy_body_transforms(body_scratch_);
	const size_t n = std::min(body_nodes_.size(), body_scratch_.size());
	for (size_t i = 0; i < n; ++i) {
		const bf3::sim::BodyTransform &t = body_scratch_[i];
		const Quaternion q(t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]);
		const Vector3 p(t.position[0], t.position[1], t.position[2]);
		body_nodes_[i]->set_transform(Transform3D(Basis(q), p + server_bodies_offset_));
	}
}

void GameSession::update_overlay() {
	if (overlay_ != nullptr) {
		overlay_->set_text(get_status_text());
	}
}

void GameSession::_physics_process(double p_delta) {
	(void)p_delta;
	physics_frames_++;
	if (selftest_) {
		selftest_->physics_tick();
	}
}

void GameSession::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || quitting_) {
		return;
	}
	elapsed_ += p_delta;
	frames_++;
	update_server_body_nodes();
	overlay_timer_ += p_delta;
	if (overlay_timer_ >= 0.1) {
		overlay_timer_ = 0.0;
		update_overlay();
	}
	if (selftest_ && selftest_->process(p_delta)) {
		quitting_ = true;
		const int code = selftest_->exit_code();
		stop_server();
		get_tree()->quit(code);
		return;
	}
	if (quit_after_ > 0.0 && !mid_screenshot_done_ && !screenshot_path_.empty() && elapsed_ >= 0.5 * quit_after_) {

		mid_screenshot_done_ = true;
		std::string mid = screenshot_path_;
		const size_t dot = mid.rfind('.');
		mid = (dot == std::string::npos ? mid : mid.substr(0, dot)) + "_mid.png";
		save_screenshot(mid);
	}
	if (quit_after_ > 0.0 && elapsed_ >= quit_after_) {
		quitting_ = true;
		save_screenshot_and_quit();
	}
}

void GameSession::save_screenshot(const std::string &p_path) {
	update_overlay();
	const std::filesystem::path target = std::filesystem::path(std::u8string(p_path.begin(), p_path.end()));
	const bf3::core::PathGuard guard = install_.write_guard();

	if (!guard.allows_write(target)) {
		UtilityFunctions::push_error(to_godot("[openbf3] screenshot refused (" +
				std::string(bf3::core::PathGuard::verdict_name(guard.check_write_target(target))) + "): " + p_path));
		return;
	}
	Ref<Image> img = get_viewport()->get_texture()->get_image();
	if (img.is_valid() && img->save_png(to_godot(p_path)) == OK) {
		UtilityFunctions::print(to_godot("[openbf3] screenshot " + p_path + " (" + std::to_string(img->get_width()) + "x" +
				std::to_string(img->get_height()) + ") at " + std::to_string(elapsed_) + " s, server tick " +
				std::to_string(server_stats().ticks)));
	} else {
		UtilityFunctions::push_error(to_godot("[openbf3] screenshot failed: " + p_path));
	}
}

void GameSession::save_screenshot_and_quit() {
	if (!screenshot_path_.empty()) {
		save_screenshot(screenshot_path_);
	}
	UtilityFunctions::print(to_godot("[openbf3] quit after " + std::to_string(elapsed_) + " s, " + std::to_string(frames_) + " frames"));
	UtilityFunctions::print(get_status_text());
	stop_server();
	get_tree()->quit(0);
}

}
