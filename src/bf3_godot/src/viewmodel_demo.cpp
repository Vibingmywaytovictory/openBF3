#include "viewmodel_demo.h"

#include "godot_util.h"
#include "m9_reload_check.h"
#include "render_bridge.h"

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sstream>

namespace bf3::godot_side {

using namespace godot;

namespace {

std::vector<double> parse_times(const std::string &p_list) {
	std::vector<double> out;
	std::stringstream ss(p_list);
	std::string item;
	while (std::getline(ss, item, ',')) {
		if (!item.empty()) {
			out.push_back(std::atof(item.c_str()));
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::string shot_path(const std::string &p_base, double p_time) {
	const size_t dot = p_base.rfind('.');
	char suffix[32];
	std::snprintf(suffix, sizeof(suffix), "_t%.2f.png", p_time);
	return (dot == std::string::npos ? p_base : p_base.substr(0, dot)) + suffix;
}

} // namespace

Bf3ViewmodelDemo::Bf3ViewmodelDemo() = default;

Bf3ViewmodelDemo::~Bf3ViewmodelDemo() {
	join_loader();
}

void Bf3ViewmodelDemo::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_status_text"), &Bf3ViewmodelDemo::get_status_text);
}

void Bf3ViewmodelDemo::_notification(int p_what) {
	// The loader thread must not outlive the node (it writes into it).
	if (p_what == NOTIFICATION_PREDELETE) {
		join_loader();
	}
}

void Bf3ViewmodelDemo::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

void Bf3ViewmodelDemo::_ready() {
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
	UtilityFunctions::print(to_godot("[viewmodel] engine " + engine_summary()));
	UtilityFunctions::print(to_godot("[viewmodel] install " + install_.describe()));

	check_mode_ = has_user_arg("m9-reload-check");
	screenshot_path_ = user_arg_value("screenshot");
	shot_times_ = parse_times(user_arg_value("shot-times"));
	const std::string quit_after = user_arg_value("quit-after");
	quit_after_ = quit_after.empty() ? 0.0 : std::atof(quit_after.c_str());
	const std::string speed = user_arg_value("clip-speed");
	clip_speed_ = speed.empty() ? 1.0 : std::max(0.0, std::atof(speed.c_str()));
	build_ui();

	if (!install_.is_mounted()) {
		load_error_ = "no usable install: " + install_.describe();
		load_done_ = true;
	} else {
		load_start_usec_ = Time::get_singleton()->get_ticks_usec();
		// Heavy work off the main thread (plan §2.3): no Godot API is called on this thread.
		loader_ = std::thread([this]() {
			bf3::core::Result<std::unique_ptr<bf3::render::ViewmodelAssets>> r = bf3::render::load_viewmodel(install_, spec_, &progress_);
			if (r) {
				loaded_ = std::move(*r);
			} else {
				load_error_ = r.error().message;
			}
			load_done_ = true;
		});
	}
	set_process(true);
}

void Bf3ViewmodelDemo::build_ui() {
	ui_layer_ = memnew(CanvasLayer);
	ui_layer_->set_name("ViewmodelUI");
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

String Bf3ViewmodelDemo::get_status_text() const {
	char buf[512];
	if (!loaded()) {
		if (!load_error_.empty()) {
			return to_godot("openBF3 viewmodel: load failed: " + load_error_);
		}
		const int step = progress_.step();
		std::snprintf(buf, sizeof(buf), "openBF3 viewmodel: loading from the install (%d/%d) %s...", step, bf3::render::LoadProgress::kSteps,
				progress_.stage().c_str());
		return to_godot(buf);
	}
	const double duration = assets_->tracks.duration_seconds();
	const double t = duration > 0.0 ? std::fmod(clip_time_, duration) : 0.0;
	std::snprintf(buf, sizeof(buf), "openBF3 | M9 \"%s\" t %.2f / %.2f s (frame %.1f) | %.0f fps | loaded in %.2f s + upload %.2f s | 1P FOV %.0f",
			spec_.clip.c_str(), t, duration, t * assets_->tracks.fps, Engine::get_singleton()->get_frames_per_second(), load_seconds_, upload_seconds_,
			assets_->render_fov > 0.0f ? assets_->render_fov : 55.0f);
	return to_godot(buf);
}

void Bf3ViewmodelDemo::update_label() {
	if (label_ != nullptr) {
		label_->set_text(get_status_text());
	}
}

void Bf3ViewmodelDemo::build_scene() {
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	bf3::render::ViewmodelAssets &a = *assets_;
	for (const std::string &n : a.notes) {
		UtilityFunctions::print(to_godot("[viewmodel] " + n));
	}

	// Textures (main thread: ImageTexture creation reaches the RenderingServer).
	std::map<std::string, Ref<Texture2D>> textures;
	for (const bf3::render::PreparedTexture &t : a.textures) {
		const Ref<ImageTexture> tex = make_texture(t);
		if (tex.is_valid()) {
			textures[t.name] = tex;
		}
	}
	auto find_texture = [&](const std::string &p_name) -> Ref<Texture2D> {
		const auto it = textures.find(p_name);
		return it == textures.end() ? Ref<Texture2D>() : it->second;
	};

	skeleton_ = make_skeleton(a.rig);
	skeleton_->set_name("RigSkeleton");
	add_child(skeleton_);
	skin_ = make_skin(a.binds.binds());
	camera_joint_ = a.rig.find("CameraJoint");

	MaterialOptions options;
	options.viewmodel = true;
	options.fov_override = a.render_fov > 0.0f ? a.render_fov : 55.0f;
	for (const bf3::render::ViewmodelPart *part : { &a.arms, &a.weapon }) {
		std::map<uint32_t, Ref<StandardMaterial3D>> materials;
		const Ref<ArrayMesh> mesh = make_array_mesh(part->prepared, [&](const bf3::render::PreparedSurface &p_s) -> Ref<Material> {
			if (p_s.material_index >= part->materials.slots.size()) {
				return Ref<Material>();
			}
			Ref<StandardMaterial3D> &m = materials[p_s.material_index];
			if (m.is_null()) {
				m = make_standard_material(part->materials.slots[p_s.material_index], !p_s.tangents.empty(), find_texture, options);
			}
			return m;
		});
		MeshInstance3D *mi = memnew(MeshInstance3D);
		const std::string short_name = part->mesh.substr(part->mesh.rfind('/') + 1);
		mi->set_name(to_godot(short_name));
		mi->set_mesh(mesh);
		mi->set_skin(skin_);
		// The bind-pose AABB says little about the animated mesh, and fov_override changes what is visible:
		// a generous box around the camera keeps the viewmodel from being culled (plan §3.3 caveat).
		mi->set_custom_aabb(AABB(Vector3(-3, -1, -3), Vector3(6, 4, 6)));
		mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		skeleton_->add_child(mi);
		mi->set_skeleton_path(NodePath(".."));
		mesh_nodes_.push_back(mi);
	}

	camera_ = memnew(Camera3D);
	camera_->set_name("ViewCamera");
	camera_->set_fov(75.0f); // world FOV; the viewmodel materials draw with their own (fov_override)
	camera_->set_near(0.05f);
	camera_->set_far(500.0f);
	add_child(camera_);
	camera_->make_current();
	pose_at_frame(0.0f);
	place_reference_props();
	RenderingServer::get_singleton()->viewport_set_measure_render_time(get_viewport()->get_viewport_rid(), true);
	upload_seconds_ = double(Time::get_singleton()->get_ticks_usec() - t0) / 1e6;
	char line[256];
	std::snprintf(line, sizeof(line), "[viewmodel] built: %zu bones, %zu skin binds, %zu textures, arms %zu/%zu, M9 %zu/%zu vertices/triangles; upload %.3f s",
			a.rig.size(), a.binds.size(), textures.size(), a.arms.prepared.vertices, a.arms.prepared.triangles, a.weapon.prepared.vertices,
			a.weapon.prepared.triangles, upload_seconds_);
	UtilityFunctions::print(line);
}

void Bf3ViewmodelDemo::place_reference_props() {
	// A floor and a few boxes in front of the soldier, so the world FOV and the viewmodel FOV can be compared.
	Ref<StandardMaterial3D> floor_mat;
	floor_mat.instantiate();
	floor_mat->set_albedo(Color(0.36f, 0.37f, 0.34f));
	Ref<PlaneMesh> plane;
	plane.instantiate();
	plane->set_size(Vector2(60, 60));
	plane->set_material(floor_mat);
	MeshInstance3D *floor = memnew(MeshInstance3D);
	floor->set_name("Floor");
	floor->set_mesh(plane);
	add_child(floor);

	const Transform3D cam = camera_->get_global_transform();
	Vector3 fwd = -cam.basis.get_column(2);
	fwd.y = 0.0f;
	fwd = fwd.length() > 1e-4f ? fwd.normalized() : Vector3(0, 0, -1);
	const Vector3 right = fwd.cross(Vector3(0, 1, 0)).normalized();
	const Vector3 base(cam.origin.x, 0.0f, cam.origin.z);
	const float spots[][3] = { { 3.0f, -1.6f, 1.2f }, { 4.5f, 1.8f, 2.0f }, { 7.0f, -0.4f, 3.0f }, { 10.0f, 3.0f, 1.5f }, { 2.2f, 2.2f, 0.8f } };
	for (int i = 0; i < 5; ++i) {
		Ref<StandardMaterial3D> m;
		m.instantiate();
		m->set_albedo(Color::from_hsv(0.08f + 0.17f * float(i), 0.45f, 0.75f));
		Ref<BoxMesh> box;
		box.instantiate();
		box->set_size(Vector3(0.8f, spots[i][2], 0.8f));
		box->set_material(m);
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_name(to_godot("RefBox" + std::to_string(i)));
		mi->set_mesh(box);
		add_child(mi);
		mi->set_global_position(base + fwd * spots[i][0] + right * spots[i][1] + Vector3(0, spots[i][2] * 0.5f, 0));
	}
}

void Bf3ViewmodelDemo::pose_at_frame(float p_frame, std::vector<bf3::anim::Xform> *r_model) {
	const bf3::render::ViewmodelAssets &a = *assets_;
	bf3::anim::sample_pose(a.rig, a.clip.clip, a.tracks, p_frame, pose_, scratch_);
	for (const int32_t b : a.tracks.rotation_bone) {
		if (b >= 0) {
			skeleton_->set_bone_pose_rotation(b, to_godot(pose_.joints[size_t(b)].q));
		}
	}
	for (const int32_t b : a.tracks.translation_bone) {
		if (b >= 0) {
			skeleton_->set_bone_pose_position(b, to_godot(pose_.joints[size_t(b)].t));
		}
	}
	bf3::anim::model_pose(a.rig, pose_, model_);
	if (camera_ != nullptr && camera_joint_ >= 0) {
		// Frostbite cameras look down +Z, Godot's down -Z: turn the CameraJoint frame 180 deg about its Y.
		const Transform3D joint = skeleton_->get_global_transform() * to_godot(model_[size_t(camera_joint_)]);
		camera_->set_global_transform(joint * Transform3D(Basis(Vector3(0, 1, 0), real_t(Math::PI)), Vector3()));
	}
	if (r_model != nullptr) {
		*r_model = model_;
	}
}

void Bf3ViewmodelDemo::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || quitting_) {
		return;
	}
	if (!loaded()) {
		if (load_done_) {
			join_loader();
			if (!load_error_.empty() || !loaded_) {
				UtilityFunctions::push_error(to_godot("[viewmodel] load failed: " + load_error_));
				update_label();
				failed_ = true;
				if (check_mode_ || quit_after_ > 0.0) {
					finish(1);
				}
				return;
			}
			load_seconds_ = double(Time::get_singleton()->get_ticks_usec() - load_start_usec_) / 1e6;
			assets_ = std::move(loaded_);
			UtilityFunctions::print(to_godot("[viewmodel] loaded in " + std::to_string(load_seconds_) + " s (ant " + std::to_string(assets_->seconds_ant) +
					", ebx " + std::to_string(assets_->seconds_ebx) + ", meshes " + std::to_string(assets_->seconds_meshes) + ", textures " +
					std::to_string(assets_->seconds_textures) + ")"));
			build_scene();
			if (check_mode_) {
				finish(run_m9_reload_check(*this));
				return;
			}
		}
		update_label();
		return;
	}

	// Playback.
	if (hold_frames_ > 0) {
		if (--hold_frames_ == 0) {
			if (!screenshot_path_.empty()) {
				save_screenshot(shot_path(screenshot_path_, shot_times_[next_shot_]));
			}
			++next_shot_;
		}
	} else {
		clip_time_ += p_delta * clip_speed_;
		if (next_shot_ < shot_times_.size() && clip_time_ >= shot_times_[next_shot_]) {
			clip_time_ = shot_times_[next_shot_];
			hold_frames_ = 4; // pose set this frame, captured after it has been drawn
		}
	}
	const double duration = assets_->tracks.duration_seconds();
	const double t = duration > 0.0 ? std::fmod(clip_time_, duration) : 0.0;
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec(); // sampling, pose, bone upload, camera (no capture)
	pose_at_frame(float(t * assets_->tracks.fps));
	animate_us_total_ += double(Time::get_singleton()->get_ticks_usec() - t0);

	play_elapsed_ += p_delta;
	frames_++;
	frame_seconds_ += p_delta;
	frame_ms_.push_back(float(p_delta * 1000.0));
	label_timer_ += p_delta;
	if (label_timer_ >= 0.1 || hold_frames_ > 0) {
		label_timer_ = 0.0;
		update_label();
	}
	const bool shots_done = next_shot_ >= shot_times_.size();
	if (quit_after_ > 0.0 && play_elapsed_ >= quit_after_ && (shots_done || play_elapsed_ >= quit_after_ * 3.0)) {
		if (!screenshot_path_.empty() && shot_times_.empty()) {
			save_screenshot(screenshot_path_);
		}
		finish(failed_ ? 1 : 0);
	}
}

void Bf3ViewmodelDemo::save_screenshot(const std::string &p_path) {
	const std::filesystem::path target = std::filesystem::path(std::u8string(p_path.begin(), p_path.end()));
	const bf3::core::PathGuard guard = install_.write_guard();
	if (!guard.allows_write(target)) {
		UtilityFunctions::push_error(to_godot("[viewmodel] screenshot refused (" +
				std::string(bf3::core::PathGuard::verdict_name(guard.check_write_target(target))) + "): " + p_path));
		failed_ = true;
		return;
	}
	update_label();
	Ref<Image> img = get_viewport()->get_texture()->get_image();
	if (img.is_valid() && img->save_png(to_godot(p_path)) == OK) {
		const double duration = assets_ ? assets_->tracks.duration_seconds() : 0.0;
		const double t = duration > 0.0 ? std::fmod(clip_time_, duration) : 0.0;
		char line[512];
		std::snprintf(line, sizeof(line), "[viewmodel] screenshot %s (%dx%d) at clip %.3f s (frame %.2f)", p_path.c_str(), img->get_width(), img->get_height(), t,
				t * (assets_ ? assets_->tracks.fps : 30.0f));
		UtilityFunctions::print(line);
	} else {
		UtilityFunctions::push_error(to_godot("[viewmodel] screenshot failed: " + p_path));
		failed_ = true;
	}
}

void Bf3ViewmodelDemo::finish(int p_code) {
	quitting_ = true;
	if (frames_ > 0) {
		std::vector<float> ms(frame_ms_.begin() + std::min<size_t>(frame_ms_.size(), 30), frame_ms_.end()); // skip warm-up frames
		if (ms.empty()) {
			ms = frame_ms_;
		}
		std::sort(ms.begin(), ms.end());
		double sum = 0.0;
		for (float v : ms) {
			sum += v;
		}
		const RID vp = get_viewport()->get_viewport_rid();
		char line[512];
		std::snprintf(line, sizeof(line),
				"[viewmodel] %llu frames in %.2f s: frame time mean %.2f ms, median %.2f ms, p99 %.2f ms (after 30 warm-up frames); animation + bone "
				"upload %.1f us/frame; RenderingServer viewport CPU %.2f ms, GPU %.2f ms",
				static_cast<unsigned long long>(frames_), frame_seconds_, sum / double(ms.size()), ms[ms.size() / 2], ms[std::min(ms.size() - 1, ms.size() * 99 / 100)],
				animate_us_total_ / double(frames_), RenderingServer::get_singleton()->viewport_get_measured_render_time_cpu(vp),
				RenderingServer::get_singleton()->viewport_get_measured_render_time_gpu(vp));
		UtilityFunctions::print(line);
	}
	if (install_before_.valid) {
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(install_before_, install_.snapshot());
		UtilityFunctions::print(to_godot(std::string("[viewmodel] install unchanged: ") + (d.clean() ? "yes" : "NO: " + d.summary())));
		if (!d.clean()) {
			p_code = 1;
		}
	}
	join_loader();
	get_tree()->quit(p_code);
}

} // namespace bf3::godot_side
