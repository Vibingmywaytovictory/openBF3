#include "collision_overlay.h"

#include "collision_check.h"
#include "godot_util.h"

#include "bf3/core/install.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace bf3::godot_side {

using namespace godot;
namespace lvl = bf3::core::level;

namespace {

static_assert(sizeof(Vector3) == 12 && sizeof(Vector2) == 8 && sizeof(Color) == 16, "single-precision Godot types");

constexpr const char *kShader = R"(shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;
uniform int mode = 0; // 0 solid, 1 solid + wireframe, 2 wireframe
uniform vec3 wire_color : source_color = vec3(0.02, 0.02, 0.03);
uniform float wire_width = 1.1;
uniform float wire_fade = 120.0; // metres: the overlay wireframe is gone at this distance
varying vec3 bary;
void vertex() {
	bary = vec3(UV, 1.0 - UV.x - UV.y);
}
void fragment() {
	vec3 d = fwidth(bary);
	vec3 a = smoothstep(vec3(0.0), d * wire_width, bary);
	float edge = 1.0 - min(min(a.x, a.y), a.z);
	float near = clamp(1.0 - length(VERTEX) / wire_fade, 0.0, 1.0);
	vec3 base = COLOR.rgb * (FRONT_FACING ? 1.0 : 0.55);
	if (mode == 2) {
		if (edge < 0.5) {
			discard;
		}
		ALBEDO = base;
	} else if (mode == 1) {
		ALBEDO = mix(base, wire_color, edge * 0.8 * near);
	} else {
		ALBEDO = base;
	}
}
)";

const char *kWireNames[3] = { "solid", "solid + wireframe", "wireframe" };
const char *kLayerNames[bf3::render::kCollisionLayers] = { "collision", "raycast", "character", "invisible", "terrain" };

bool parse_floats(const std::string &p_text, size_t p_count, float *r_out) {
	std::stringstream ss(p_text);
	std::string item;
	size_t n = 0;
	while (std::getline(ss, item, ',')) {
		if (n >= p_count || item.empty()) {
			return false;
		}
		char *end = nullptr;
		r_out[n++] = std::strtof(item.c_str(), &end);
		if (end == item.c_str()) {
			return false;
		}
	}
	return n == p_count;
}

std::string with_commas(size_t p_value) {
	std::string digits = std::to_string(p_value);
	for (int at = int(digits.size()) - 3; at > 0; at -= 3) {
		digits.insert(size_t(at), ",");
	}
	return digits;
}

}

Bf3CollisionOverlay::Bf3CollisionOverlay() = default;

Bf3CollisionOverlay::~Bf3CollisionOverlay() {
	join_loader();
	join_window();
}

void Bf3CollisionOverlay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_status_text"), &Bf3CollisionOverlay::get_status_text);
}

void Bf3CollisionOverlay::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE) {
		join_loader();
		join_window();
		free_render_objects();
	}
}

void Bf3CollisionOverlay::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

void Bf3CollisionOverlay::join_window() {
	if (window_thread_.joinable()) {
		window_thread_.join();
	}
}

void Bf3CollisionOverlay::free_mesh(TileMesh &r_mesh) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	if (r_mesh.instance.is_valid()) {
		rs->free_rid(r_mesh.instance);
		r_mesh.instance = RID();
	}
	if (r_mesh.mesh.is_valid()) {
		rs->free_rid(r_mesh.mesh);
		r_mesh.mesh = RID();
	}
}

void Bf3CollisionOverlay::update_window() {
	if (window_running_) {
		if (!window_done_.load()) {
			return;
		}
		join_window();
		window_running_ = false;

		for (TileMesh &m : meshes_) {
			if (m.layer == bf3::render::kTerrainLayer) {
				free_mesh(m);
			}
		}
		std::erase_if(meshes_, [](const TileMesh &m) { return m.layer == bf3::render::kTerrainLayer; });
		std::erase_if(prepared_, [](const bf3::render::PreparedCollisionMesh &m) { return m.layer == bf3::render::kTerrainLayer; });
		next_upload_ = prepared_.size();
		for (bf3::render::PreparedCollisionMesh &m : window_loaded_) {
			prepared_.push_back(std::move(m));
		}
		window_loaded_.clear();
		window_ = window_loaded_stats_;
		window_options_ = window_job_options_;
		window_valid_ = true;
		const double before = upload_seconds_;
		upload_some(true);
		char line[384];
		std::snprintf(line, sizeof(line),
				"[collision] terrain window %zu: %zu tiles, %zu triangles within %.0f m of (%.1f, %.1f)%s%s; prepared %.3f s, uploaded %.3f s",
				window_jobs_, window_.tiles, window_.triangles, double(window_.radius), double(window_.centre[0]), double(window_.centre[1]),
				window_.capped ? ", capped at max_terrain_triangles" : "", window_.tiles_failed > 0 ? ", some tiles failed" : "", window_seconds_,
				upload_seconds_ - before);
		UtilityFunctions::print(line);
	}
	const float dx = position_.x - window_options_.terrain_centre[0], dz = position_.z - window_options_.terrain_centre[1];
	if (window_valid_ && std::sqrt(dx * dx + dz * dz) <= 0.5f * window_options_.terrain_radius) {
		return;
	}
	window_job_options_ = bf3::render::CollisionPrepOptions();
	window_job_options_.terrain_centre = { position_.x, position_.z };
	window_job_options_.terrain_radius = terrain_radius_;
	window_running_ = true;
	window_done_ = false;
	++window_jobs_;

	window_thread_ = std::thread([this]() {
		const auto t0 = std::chrono::steady_clock::now();
		window_loaded_ = bf3::render::prepare_terrain_window(collision_->soup, window_job_options_, &window_loaded_stats_);
		window_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		window_done_ = true;
	});
}

void Bf3CollisionOverlay::free_render_objects() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	for (TileMesh &m : meshes_) {
		if (m.instance.is_valid()) {
			rs->free_rid(m.instance);
		}
		if (m.mesh.is_valid()) {
			rs->free_rid(m.mesh);
		}
	}
	meshes_.clear();
	if (material_.is_valid()) {
		rs->free_rid(material_);
		material_ = RID();
	}
	if (shader_.is_valid()) {
		rs->free_rid(shader_);
		shader_ = RID();
	}
}

void Bf3CollisionOverlay::_ready() {
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
	UtilityFunctions::print(to_godot("[collision] engine " + engine_summary()));
	UtilityFunctions::print(to_godot("[collision] install " + install_.describe()));

	if (!user_arg_value("level").empty()) {
		level_ = user_arg_value("level");
	}
	if (!user_arg_value("mode").empty()) {
		mode_ = user_arg_value("mode");
	}
	check_mode_ = has_user_arg("collision-check");
	screenshot_path_ = user_arg_value("screenshot");
	const std::string quit_after = user_arg_value("quit-after");
	quit_after_ = quit_after.empty() ? 0.0 : std::atof(quit_after.c_str());
	start_camera_ = user_arg_value("camera");
	start_view_ = user_arg_value("view");
	if (has_user_arg("terrain-radius")) {
		const float r = float(std::atof(user_arg_value("terrain-radius").c_str()));
		terrain_radius_ = std::isfinite(r) ? std::clamp(r, 0.0f, bf3::render::kMaxTerrainWindowRadius) : terrain_radius_;
	}
	const std::string wire = user_arg_value("wire");
	if (!wire.empty()) {
		wire_mode_ = std::clamp(std::atoi(wire.c_str()), 0, 2);
	}
	if (has_user_arg("layers")) {
		const std::string layers = user_arg_value("layers");
		for (size_t b = 0; b < layers_.size(); ++b) {
			layers_[b] = layers.find(char('1' + b)) != std::string::npos;
		}
	}
	{
		std::stringstream ss(user_arg_value("shots"));
		std::string item;
		while (std::getline(ss, item, ';')) {
			const size_t at = item.rfind('@');
			Shot shot;
			if (at == std::string::npos || !parse_floats(item.substr(at + 1), 5, shot.camera.data())) {
				UtilityFunctions::push_error(to_godot("[collision] --shots entry not <png>@x,y,z,yaw,pitch: " + item));
				failed_ = true;
				continue;
			}
			shot.path = item.substr(0, at);
			shots_.push_back(shot);
		}
	}
	build_ui();

	if (!install_.is_mounted()) {
		load_error_ = "no usable install: " + install_.describe();
		load_done_ = true;
	} else {
		load_start_usec_ = Time::get_singleton()->get_ticks_usec();

		loader_ = std::thread([this]() {
			bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(install_);
			if (!install) {
				load_error_ = install.error().message;
				load_done_ = true;
				return;
			}
			opened_ = *install;
			lvl::SoupOptions options;
			options.progress = &progress_;
			bf3::core::Result<lvl::LevelCollision> r = lvl::load_level_collision(*install, level_, mode_, options);
			if (!r) {
				load_error_ = r.error().message;
				load_done_ = true;
				return;
			}
			loaded_ = std::make_unique<lvl::LevelCollision>(std::move(*r));
			preparing_ = true;
			const auto t0 = std::chrono::steady_clock::now();
			loaded_meshes_ = bf3::render::prepare_collision_meshes(loaded_->soup);
			loader_prepare_seconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			load_done_ = true;
		});
	}
	set_process(true);
	set_process_unhandled_input(true);
}

void Bf3CollisionOverlay::build_ui() {
	ui_layer_ = memnew(CanvasLayer);
	ui_layer_->set_name("CollisionUI");
	label_ = memnew(Label);
	label_->set_name("Status");
	label_->set_position(Vector2(12, 8));
	label_->add_theme_color_override("font_color", Color(1, 1, 1));
	label_->add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.9f));
	label_->add_theme_constant_override("shadow_offset_x", 1);
	label_->add_theme_constant_override("shadow_offset_y", 1);
	label_->add_theme_font_size_override("font_size", 15);
	ui_layer_->add_child(label_);
	add_child(ui_layer_);
	update_label();
}

String Bf3CollisionOverlay::get_status_text() const {
	char buf[1024];
	if (!collision_) {
		if (!load_error_.empty()) {
			return to_godot("openBF3 collision: load failed: " + load_error_);
		}
		const int stage = progress_.stage.load();
		const size_t done = progress_.done.load(), total = progress_.total.load();
		if (preparing_) {
			std::snprintf(buf, sizeof(buf), "openBF3 collision: %s %s: preparing tile meshes...", level_.c_str(), mode_.c_str());
		} else if (total > 0) {
			std::snprintf(buf, sizeof(buf), "openBF3 collision: %s %s: %s %zu/%zu...", level_.c_str(), mode_.c_str(),
					std::string(lvl::soup_stage_name(stage)).c_str(), done, total);
		} else {
			std::snprintf(buf, sizeof(buf), "openBF3 collision: %s %s: %s...", level_.c_str(), mode_.c_str(), std::string(lvl::soup_stage_name(stage)).c_str());
		}
		return to_godot(buf);
	}
	const lvl::CollisionSoup &s = collision_->soup;
	if (!uploaded()) {
		std::snprintf(buf, sizeof(buf), "openBF3 collision: %s %s: uploading tile meshes %zu/%zu...", level_.c_str(), mode_.c_str(), next_upload_,
				prepared_.size());
		return to_godot(buf);
	}
	const Vector3 p = position_;
	std::snprintf(buf, sizeof(buf),
			"openBF3 collision | %s %s | %s placements | %s Havok + %s terrain triangles | %zu tiles (%zu meshes)\n"
			"terrain window: %zu tiles, %s triangles within %.0f m of (%.0f, %.0f)%s%s\n"
			"load %.2f s + prepare %.2f s + upload %.2f s | %.0f fps | %s | pos %.1f %.1f %.1f | speed %.0f m/s\n"
			"layers: [1] collision %s  [2] raycast %s  [3] character %s  [4] invisible %s  [5] terrain %s\n"
			"WASD move, Q/E down/up, Shift fast, wheel speed, RMB look, F3 draw mode, 1-5 layers, R spawn, N next spawn, T top",
			level_.c_str(), mode_.c_str(), with_commas(s.placements.size()).c_str(), with_commas(s.triangles).c_str(), with_commas(s.terrain_triangles).c_str(),
			s.tiles.size(), meshes_.size(), window_.tiles, with_commas(window_.triangles).c_str(), double(window_.radius), double(window_.centre[0]),
			double(window_.centre[1]), window_.capped ? " (capped)" : "", window_running_ ? " (updating)" : "", load_seconds_, prepare_seconds_, upload_seconds_, Engine::get_singleton()->get_frames_per_second(),
			kWireNames[wire_mode_], p.x, p.y, p.z, speed_, layers_[0] ? "on" : "off", layers_[1] ? "on" : "off", layers_[2] ? "on" : "off",
			layers_[3] ? "on" : "off", layers_[4] ? "on" : "off");
	return to_godot(buf);
}

void Bf3CollisionOverlay::update_label() {
	if (label_ != nullptr) {
		label_->set_text(get_status_text());
	}
}

void Bf3CollisionOverlay::create_material() {
	RenderingServer *rs = RenderingServer::get_singleton();
	shader_ = rs->shader_create();
	rs->shader_set_code(shader_, kShader);
	material_ = rs->material_create();
	rs->material_set_shader(material_, shader_);
	apply_wire();
}

void Bf3CollisionOverlay::upload_some(bool p_all) {
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID scenario = get_world_3d()->get_scenario();
	const uint64_t start = Time::get_singleton()->get_ticks_usec();
	while (next_upload_ < prepared_.size()) {
		const bf3::render::PreparedCollisionMesh &m = prepared_[next_upload_];
		const int64_t vertices = int64_t(m.positions.size() / 3);
		PackedVector3Array positions;
		positions.resize(vertices);
		std::memcpy(positions.ptrw(), m.positions.data(), size_t(vertices) * sizeof(Vector3));
		PackedColorArray colors;
		colors.resize(vertices);
		std::memcpy(colors.ptrw(), m.colors.data(), size_t(vertices) * sizeof(Color));
		PackedVector2Array uvs;
		uvs.resize(vertices);
		std::memcpy(uvs.ptrw(), m.uvs.data(), size_t(vertices) * sizeof(Vector2));
		Array arrays;
		arrays.resize(RenderingServer::ARRAY_MAX);
		arrays[RenderingServer::ARRAY_VERTEX] = positions;
		arrays[RenderingServer::ARRAY_COLOR] = colors;
		arrays[RenderingServer::ARRAY_TEX_UV] = uvs;
		TileMesh tm;
		tm.tile_x = m.tile_x;
		tm.tile_z = m.tile_z;
		tm.layer = m.layer;
		tm.triangles = m.triangles;
		tm.mesh = rs->mesh_create();
		rs->mesh_add_surface_from_arrays(tm.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
		rs->mesh_surface_set_material(tm.mesh, 0, material_);
		tm.instance = rs->instance_create2(tm.mesh, scenario);
		rs->instance_geometry_set_cast_shadows_setting(tm.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
		rs->instance_set_visible(tm.instance, layers_[std::min<size_t>(tm.layer, layers_.size() - 1)]);
		meshes_.push_back(tm);
		++next_upload_;
		if (!p_all && Time::get_singleton()->get_ticks_usec() - start > 12000) {
			break;
		}
	}
	upload_seconds_ += double(Time::get_singleton()->get_ticks_usec() - start) / 1e6;
}

void Bf3CollisionOverlay::apply_layers() {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (const TileMesh &m : meshes_) {
		rs->instance_set_visible(m.instance, layers_[std::min<size_t>(m.layer, layers_.size() - 1)]);
	}
}

void Bf3CollisionOverlay::apply_wire() {
	if (material_.is_valid()) {
		RenderingServer::get_singleton()->material_set_param(material_, "mode", wire_mode_);
	}
}

void Bf3CollisionOverlay::set_camera(const std::array<float, 5> &p_camera) {
	position_ = Vector3(p_camera[0], p_camera[1], p_camera[2]);
	yaw_ = float(Math::deg_to_rad(double(p_camera[3])));
	pitch_ = float(Math::deg_to_rad(double(std::clamp(p_camera[4], -89.0f, 89.0f))));
	apply_camera();
}

void Bf3CollisionOverlay::spawn_camera(size_t p_index) {
	const std::vector<lvl::LevelTransform> &spawns = collision_->spawns;
	if (spawns.empty()) {
		top_camera();
		return;
	}
	spawn_index_ = p_index % spawns.size();
	const lvl::LevelTransform &t = spawns[spawn_index_];

	position_ = Vector3(t.trans[0], t.trans[1] + 1.7f, t.trans[2]);
	yaw_ = std::atan2(-t.forward[0], -t.forward[2]);
	pitch_ = -0.12f;
	apply_camera();
}

void Bf3CollisionOverlay::top_camera() {

	const lvl::CollisionSoup &s = collision_->soup;
	const auto quantiles = [&](int p_axis, float &r_lo, float &r_hi) {
		std::vector<std::pair<int32_t, size_t>> keys;
		for (const lvl::SoupTile &t : s.tiles) {
			keys.emplace_back(p_axis == 0 ? t.x : t.z, t.triangles.size());
		}
		std::sort(keys.begin(), keys.end());
		size_t acc = 0;
		r_lo = r_hi = 0.0f;
		bool lo_set = false;
		for (const auto &[key, count] : keys) {
			acc += count;
			if (!lo_set && acc >= s.triangles / 100) {
				r_lo = 64.0f * float(key);
				lo_set = true;
			}
			if (acc <= s.triangles - s.triangles / 100) {
				r_hi = 64.0f * float(key + 1);
			}
		}
		r_hi = std::max(r_hi, r_lo + 64.0f);
	};
	float x0 = s.min[0], x1 = s.max[0], z0 = s.min[2], z1 = s.max[2];
	if (!s.tiles.empty()) {
		quantiles(0, x0, x1);
		quantiles(2, z0, z1);
	}
	const float cx = 0.5f * (x0 + x1), cz = 0.5f * (z0 + z1);
	const float extent = std::max(x1 - x0, z1 - z0);
	position_ = Vector3(cx, s.max[1] + std::max(50.0f, 0.95f * extent), cz);
	yaw_ = 0.0f;
	pitch_ = float(Math::deg_to_rad(-89.0));
	apply_camera();
}

void Bf3CollisionOverlay::place_start_camera() {
	std::array<float, 5> c{};
	if (!start_camera_.empty() && parse_floats(start_camera_, 5, c.data())) {
		set_camera(c);
	} else if (start_view_ == "top") {
		top_camera();
	} else {
		spawn_camera(0);
	}
}

void Bf3CollisionOverlay::apply_camera() {
	if (camera_ == nullptr) {
		return;
	}
	const Basis basis = Basis(Vector3(0, 1, 0), yaw_) * Basis(Vector3(1, 0, 0), pitch_);
	camera_->set_global_transform(Transform3D(basis, position_));
}

void Bf3CollisionOverlay::move_camera(double p_delta) {
	Input *in = Input::get_singleton();
	Vector3 dir;
	const Basis basis = Basis(Vector3(0, 1, 0), yaw_) * Basis(Vector3(1, 0, 0), pitch_);
	const Vector3 forward = -basis.get_column(2);
	const Vector3 right = basis.get_column(0);
	if (in->is_physical_key_pressed(KEY_W)) {
		dir += forward;
	}
	if (in->is_physical_key_pressed(KEY_S)) {
		dir -= forward;
	}
	if (in->is_physical_key_pressed(KEY_D)) {
		dir += right;
	}
	if (in->is_physical_key_pressed(KEY_A)) {
		dir -= right;
	}
	if (in->is_physical_key_pressed(KEY_E) || in->is_physical_key_pressed(KEY_SPACE)) {
		dir += Vector3(0, 1, 0);
	}
	if (in->is_physical_key_pressed(KEY_Q) || in->is_physical_key_pressed(KEY_CTRL)) {
		dir -= Vector3(0, 1, 0);
	}
	if (dir.length_squared() > 0.0f) {
		const float speed = speed_ * (in->is_physical_key_pressed(KEY_SHIFT) ? 6.0f : 1.0f);
		position_ += dir.normalized() * float(speed * p_delta);
		apply_camera();
	}
}

void Bf3CollisionOverlay::_unhandled_input(const Ref<InputEvent> &p_event) {
	if (!uploaded() || check_mode_) {
		return;
	}
	Input *in = Input::get_singleton();
	if (const InputEventMouseButton *mb = Object::cast_to<InputEventMouseButton>(p_event.ptr())) {
		if (mb->get_button_index() == MOUSE_BUTTON_RIGHT) {
			looking_ = mb->is_pressed();
			in->set_mouse_mode(looking_ ? Input::MOUSE_MODE_CAPTURED : Input::MOUSE_MODE_VISIBLE);
		} else if (mb->get_button_index() == MOUSE_BUTTON_LEFT && mb->is_pressed() && !looking_) {
			looking_ = true;
			in->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
		} else if (mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP && mb->is_pressed()) {
			speed_ = std::min(400.0f, speed_ * 1.25f);
		} else if (mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN && mb->is_pressed()) {
			speed_ = std::max(1.0f, speed_ / 1.25f);
		}
		return;
	}
	if (const InputEventMouseMotion *mm = Object::cast_to<InputEventMouseMotion>(p_event.ptr())) {
		if (looking_) {
			const Vector2 rel = mm->get_relative();
			yaw_ -= rel.x * 0.003f;
			pitch_ = std::clamp(pitch_ - rel.y * 0.003f, float(Math::deg_to_rad(-89.0)), float(Math::deg_to_rad(89.0)));
			apply_camera();
		}
		return;
	}
	if (const InputEventKey *key = Object::cast_to<InputEventKey>(p_event.ptr())) {
		if (!key->is_pressed() || key->is_echo()) {
			return;
		}
		switch (key->get_keycode()) {
			case KEY_F3:
				wire_mode_ = (wire_mode_ + 1) % 3;
				apply_wire();
				break;
			case KEY_1:
			case KEY_2:
			case KEY_3:
			case KEY_4:
			case KEY_5: {
				const size_t b = size_t(int64_t(key->get_keycode()) - int64_t(KEY_1));
				layers_[b] = !layers_[b];
				apply_layers();
				break;
			}
			case KEY_R:
				spawn_camera(spawn_index_);
				break;
			case KEY_N:
				spawn_camera(spawn_index_ + 1);
				break;
			case KEY_T:
				top_camera();
				break;
			case KEY_ESCAPE:
				looking_ = false;
				in->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
				break;
			default:
				return;
		}
		update_label();
	}
}

void Bf3CollisionOverlay::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || quitting_) {
		return;
	}
	if (!collision_) {
		if (load_done_) {
			join_loader();
			if (!load_error_.empty() || !loaded_) {
				UtilityFunctions::push_error(to_godot("[collision] load failed: " + load_error_));
				update_label();
				failed_ = true;
				if (check_mode_ || quit_after_ > 0.0 || !shots_.empty()) {
					finish(1);
				}
				return;
			}
			load_seconds_ = double(Time::get_singleton()->get_ticks_usec() - load_start_usec_) / 1e6 - loader_prepare_seconds_;
			prepare_seconds_ = loader_prepare_seconds_;
			collision_ = std::move(loaded_);
			prepared_ = std::move(loaded_meshes_);
			const lvl::CollisionSoup &s = collision_->soup;
			char line[640];
			std::snprintf(line, sizeof(line),
					"[collision] %s %s: %zu placements (%zu built, %zu hidden, %zu missing), %zu Havok triangles (+ %zu terrain; %zu destroyed-state, %zu degenerate "
					"dropped) in %zu tiles, %zu meshes; load %.3f s (mount %.3f, traverse %.3f, soup %.3f: decode %.3f, triangles %.3f, terrain %.3f, "
					"tiles %.3f), prepare %.3f s",
					level_.c_str(), mode_.c_str(), s.placements.size(), s.built, s.hidden, s.missing, s.triangles, s.terrain_triangles, s.destroyed, s.degenerate,
					s.tiles.size(), prepared_.size(), load_seconds_, collision_->seconds_load, collision_->seconds_traverse, collision_->seconds_soup,
					s.seconds_decode, s.seconds_triangles, s.seconds_terrain, s.seconds_tiles, prepare_seconds_);
			UtilityFunctions::print(line);
			for (const lvl::SoupPlacement &p : s.placements) {
				if (p.state == lvl::SoupPlacement::State::Missing) {
					UtilityFunctions::print(to_godot("[collision] missing: " + p.res + ": " + p.error));
				}
			}
			for (const lvl::SoupTerrain &t : s.terrains) {
				if (t.state == lvl::SoupTerrain::State::Missing) {
					UtilityFunctions::print(to_godot("[collision] terrain missing: " + t.error));
				}
			}
			camera_ = memnew(Camera3D);
			camera_->set_name("FreeCamera");
			camera_->set_fov(75.0f);
			camera_->set_near(0.1f);
			camera_->set_far(6000.0f);
			add_child(camera_);
			camera_->make_current();
			place_start_camera();
			create_material();
			RenderingServer::get_singleton()->viewport_set_measure_render_time(get_viewport()->get_viewport_rid(), true);
			upload_start_usec_ = Time::get_singleton()->get_ticks_usec();
		}
		update_label();
		return;
	}
	if (!uploaded()) {
		upload_some(check_mode_);
		if (uploaded()) {
			upload_wall_seconds_ = double(Time::get_singleton()->get_ticks_usec() - upload_start_usec_) / 1e6;
			char line[256];
			std::snprintf(line, sizeof(line), "[collision] uploaded %zu meshes / %zu instances: %.3f s on the main thread (%.3f s wall over frames)",
					meshes_.size(), meshes_.size(), upload_seconds_, upload_wall_seconds_);
			UtilityFunctions::print(line);
		}
		update_label();
		return;
	}
	update_window();
	if (check_mode_) {
		if (window_ready()) {
			finish(run_collision_check(*this));
		}
		return;
	}

	move_camera(p_delta);
	frames_++;
	frame_ms_.push_back(float(p_delta * 1000.0));
	since_upload_ += p_delta;
	label_timer_ += p_delta;
	if (label_timer_ >= 0.1) {
		label_timer_ = 0.0;
		update_label();
	}
	if (!shots_.empty()) {
		if (hold_frames_ == 0 && next_shot_ < shots_.size()) {
			set_camera(shots_[next_shot_].camera);
			update_label();
			hold_frames_ = 6;
		} else if (hold_frames_ > 0 && window_ready() && --hold_frames_ == 0) {
			if (!save_screenshot(shots_[next_shot_].path)) {
				failed_ = true;
			}
			++next_shot_;
			if (next_shot_ >= shots_.size() && (quit_after_ <= 0.0 || since_upload_ >= quit_after_)) {
				finish(failed_ ? 1 : 0);
			}
		}
		if (next_shot_ < shots_.size() || quit_after_ <= 0.0) {
			return;
		}
	}
	if (quit_after_ > 0.0 && since_upload_ >= quit_after_) {
		if (!screenshot_path_.empty()) {
			if (!save_screenshot(screenshot_path_)) {
				failed_ = true;
			}
		}
		finish(failed_ ? 1 : 0);
	}
}

bool Bf3CollisionOverlay::save_screenshot(const std::string &p_path) {
	const std::filesystem::path target = std::filesystem::path(std::u8string(p_path.begin(), p_path.end()));
	const bf3::core::PathGuard guard = install_.write_guard();
	if (!guard.allows_write(target)) {
		UtilityFunctions::push_error(to_godot("[collision] screenshot refused (" +
				std::string(bf3::core::PathGuard::verdict_name(guard.check_write_target(target))) + "): " + p_path));
		return false;
	}
	update_label();
	Ref<Image> img = get_viewport()->get_texture()->get_image();
	if (img.is_null() || img->save_png(to_godot(p_path)) != OK) {
		UtilityFunctions::push_error(to_godot("[collision] screenshot failed: " + p_path));
		return false;
	}
	char line[640];
	std::snprintf(line, sizeof(line), "[collision] screenshot %s (%dx%d) camera %.1f %.1f %.1f yaw %.1f pitch %.1f", p_path.c_str(), img->get_width(),
			img->get_height(), position_.x, position_.y, position_.z, Math::rad_to_deg(double(yaw_)), Math::rad_to_deg(double(pitch_)));
	UtilityFunctions::print(line);
	return true;
}

void Bf3CollisionOverlay::finish(int p_code) {
	quitting_ = true;
	if (frames_ > 0) {
		std::vector<float> ms(frame_ms_.begin() + std::min<size_t>(frame_ms_.size(), 30), frame_ms_.end());
		if (ms.empty()) {
			ms = frame_ms_;
		}
		std::sort(ms.begin(), ms.end());
		double sum = 0.0;
		for (float v : ms) {
			sum += v;
		}
		const RID vp = get_viewport()->get_viewport_rid();
		RenderingServer *rs = RenderingServer::get_singleton();
		char line[512];
		std::snprintf(line, sizeof(line),
				"[collision] %llu frames: frame time mean %.2f ms, median %.2f ms, p99 %.2f ms (after 30 warm-up frames); RenderingServer viewport CPU "
				"%.2f ms, GPU %.2f ms; %lld primitives, %lld draw calls in the last frame",
				static_cast<unsigned long long>(frames_), sum / double(ms.size()), ms[ms.size() / 2], ms[std::min(ms.size() - 1, ms.size() * 99 / 100)],
				rs->viewport_get_measured_render_time_cpu(vp), rs->viewport_get_measured_render_time_gpu(vp),
				static_cast<long long>(rs->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME)),
				static_cast<long long>(rs->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME)));
		UtilityFunctions::print(line);
	}
	if (install_before_.valid) {
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(install_before_, install_.snapshot());
		UtilityFunctions::print(to_godot(std::string("[collision] install unchanged: ") + (d.clean() ? "yes" : "NO: " + d.summary())));
		if (!d.clean()) {
			p_code = 1;
		}
	}
	join_loader();
	join_window();
	get_tree()->quit(p_code);
}

}
