#include "fx_ref.h"

#include "bf3/core/util/format.h"
#include "bf3/core/util/percentile.h"
#include "bf3/core/util/splitmix64.h"

#include "godot_util.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/gpu_particles3d.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/omni_light3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/particle_process_material.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace bf3::godot_side {

using namespace godot;
namespace fx = bf3::fx;

namespace {

void log(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[fx-ref] " + p_line));
}

using bf3::core::fmt;
using bf3::core::rounded_rank;
using bf3::core::splitmix64_next;

float unit(uint64_t &r_state) {
	return float(splitmix64_next(r_state) >> 40) / float(1ull << 24);
}

std::vector<Vector3> positions() {
	std::vector<Vector3> p(kFxRefParticles);
	uint64_t s = 0xF2F2;
	for (Vector3 &v : p) {
		v = Vector3(-20.0f + 40.0f * unit(s), 0.5f + 7.5f * unit(s), -20.0f + 30.0f * unit(s));
	}
	return p;
}

Ref<ImageTexture> puff_texture() {
	const int n = 256;
	PackedByteArray b;
	b.resize(n * n * 4);
	uint8_t *w = b.ptrw();
	uint64_t s = 77;
	std::vector<float> noise(16 * 16);
	for (float &x : noise) {
		x = unit(s);
	}
	for (int y = 0; y < n; ++y) {
		for (int x = 0; x < n; ++x) {
			const float u = (float(x) + 0.5f) / float(n) * 2.0f - 1.0f, v = (float(y) + 0.5f) / float(n) * 2.0f - 1.0f;
			const float r = std::sqrt(u * u + v * v);
			const float gx = (u * 0.5f + 0.5f) * 15.0f, gy = (v * 0.5f + 0.5f) * 15.0f;
			const int ix = std::min(14, int(gx)), iy = std::min(14, int(gy));
			const float fx_ = gx - float(ix), fy = gy - float(iy);
			const float a0 = noise[iy * 16 + ix] * (1 - fx_) + noise[iy * 16 + ix + 1] * fx_;
			const float a1 = noise[(iy + 1) * 16 + ix] * (1 - fx_) + noise[(iy + 1) * 16 + ix + 1] * fx_;
			const float nz = a0 * (1 - fy) + a1 * fy;
			const float a = std::clamp((1.0f - r) * 1.6f, 0.0f, 1.0f) * (0.55f + 0.45f * nz);
			uint8_t *px = w + (y * n + x) * 4;
			px[0] = px[1] = px[2] = uint8_t(200 + 55 * nz);
			px[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f);
		}
	}
	Ref<Image> img = Image::create_from_data(n, n, false, Image::FORMAT_RGBA8, b);
	img->generate_mipmaps();
	return ImageTexture::create_from_image(img);
}

Ref<ImageTexture> flat_normal() {
	PackedByteArray b;
	b.resize(4 * 4 * 4);
	for (int i = 0; i < 16; ++i) {
		b.set(i * 4 + 0, 128);
		b.set(i * 4 + 1, 128);
		b.set(i * 4 + 2, 255);
		b.set(i * 4 + 3, 255);
	}
	return ImageTexture::create_from_image(Image::create_from_data(4, 4, false, Image::FORMAT_RGBA8, b));
}

Ref<StandardMaterial3D> ref_material(const Ref<Texture2D> &p_tex, bool p_lit, bool p_particles) {
	Ref<StandardMaterial3D> m;
	m.instantiate();
	m->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	m->set_shading_mode(p_lit ? BaseMaterial3D::SHADING_MODE_PER_PIXEL : BaseMaterial3D::SHADING_MODE_UNSHADED);
	m->set_billboard_mode(p_particles ? BaseMaterial3D::BILLBOARD_PARTICLES : BaseMaterial3D::BILLBOARD_ENABLED);
	m->set_flag(BaseMaterial3D::FLAG_BILLBOARD_KEEP_SCALE, true);
	m->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
	m->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, p_tex);
	m->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	if (p_lit) {
		m->set_proximity_fade_enabled(true);
		m->set_proximity_fade_distance(1.0f);
		m->set_specular_mode(BaseMaterial3D::SPECULAR_DISABLED);
	}
	return m;
}

constexpr uint32_t kFxRefEffects = 16;
constexpr uint32_t kFxRefPerEffect = kFxRefParticles / kFxRefEffects;
std::shared_ptr<const fx::EffectDef> vfx2_def() {
	fx::EmitterDef e;
	e.name = "fx2/synthetic/ref16k";
	e.tmpl.max_count = kFxRefPerEffect;
	e.tmpl.lifetime = 0.01f;
	e.tmpl.soft_particles_fade = -1.0f;
	e.tmpl.light_multiplier = 1.0f;
	e.tmpl.light_wrap_around = 0.0f;
	e.slots[size_t(fx::ProcessorType::SpawnRate)].present = true;
	e.params.spawn_rate = float(kFxRefPerEffect - 1) * 100.0f + 1.0f;
	e.params.distribute_over_time = false;
	fx::EvaluatorDef box;
	box.type = fx::EvaluatorType::Box;
	box.box.dimensions = fx::Vec3{ 10.0f, 7.5f, 7.5f };
	box.box.pivot = fx::Vec3{ 0, 0, 0 };
	e.evaluators.push_back(box);
	e.slots[size_t(fx::ProcessorType::SpawnPosition)].present = true;
	e.slots[size_t(fx::ProcessorType::SpawnPosition)].evaluator = 0;
	e.slots[size_t(fx::ProcessorType::UpdateAge)].present = true;
	e.params.particle_lifetime = 1.0e6f;
	e.slots[size_t(fx::ProcessorType::UpdateLinearVelocity)].present = true;
	e.texture.texture = "fx2/synthetic/puff";
	auto d = std::make_shared<fx::EffectDef>();
	d->name = "fx2/synthetic/ref16k";
	d->max_instance_count = uint16_t(kFxRefEffects);
	d->reset_instance_when_started = false;
	d->emitters.push_back(std::move(e));
	fx::EffectComponentDef c;
	c.kind = fx::ComponentKind::Emitter;
	c.emitter = 0;
	d->components.push_back(c);
	d->max_particles = kFxRefPerEffect;
	return d;
}

const char *row_name(int p_row) {
	static const char *names[] = { "ref-unlit", "ref-lit-soft", "ref-lit-soft+64", "gpu-16x1000", "vfx2", "vfx2+64" };
	return names[p_row];
}

}

Bf3FxRef::Bf3FxRef() = default;

Bf3FxRef::~Bf3FxRef() {
	if (renderer_ != nullptr) {
		renderer_->clear();
	}
}

void Bf3FxRef::_bind_methods() {}

void Bf3FxRef::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	check_ = has_user_arg("fx-ref-check");
	shot_dir_ = user_arg_value("fx-ref-shot-dir");
	static_ = has_user_arg("fx-ref-static");
	no_soft_ = has_user_arg("fx-ref-nosoft");
	busy_ms_ = has_user_arg("fx-ref-busy") ? std::atof(user_arg_value("fx-ref-busy").c_str()) : 0.0;
	if (has_user_arg("fx-ref-pairs")) {
		pairs_ = uint32_t(std::clamp(std::atoi(user_arg_value("fx-ref-pairs").c_str()), 1, 20));
	}
	if (has_user_arg("fx-ref-window")) {
		window_ = std::clamp(std::atof(user_arg_value("fx-ref-window").c_str()), 2.0, 30.0);
	}
	DisplayServer *ds = DisplayServer::get_singleton();
	const int32_t primary = ds->get_primary_screen();
	ds->window_set_mode(DisplayServer::WINDOW_MODE_WINDOWED);
	ds->window_set_flag(DisplayServer::WINDOW_FLAG_BORDERLESS, true);
	ds->window_set_current_screen(primary);
	ds->window_set_position(ds->screen_get_position(primary));
	ds->window_set_size(Vector2i(1920, 1080));
	ds->window_set_vsync_mode(DisplayServer::VSYNC_DISABLED);
	Engine::get_singleton()->set_max_fps(0);
	set_process(true);
}

void Bf3FxRef::build() {
	const Ref<ImageTexture> puff = puff_texture();

	{
		Ref<Environment> env;
		env.instantiate();
		env->set_background(Environment::BG_COLOR);
		env->set_bg_color(Color(0.35f, 0.4f, 0.48f));
		env->set_ambient_source(Environment::AMBIENT_SOURCE_COLOR);
		env->set_ambient_light_color(Color(0.5f, 0.5f, 0.55f));
		WorldEnvironment *we = memnew(WorldEnvironment);
		we->set_environment(env);
		add_child(we);
		DirectionalLight3D *sun = memnew(DirectionalLight3D);
		sun->set_shadow(false);
		sun->set_rotation_degrees(Vector3(-50, 30, 0));
		add_child(sun);
		Ref<PlaneMesh> plane;
		plane.instantiate();
		plane->set_size(Vector2(300, 300));
		MeshInstance3D *floor = memnew(MeshInstance3D);
		floor->set_mesh(plane);
		add_child(floor);
	}
	camera_ = memnew(Camera3D);
	add_child(camera_);
	camera_->set_fov(70.0f);
	camera_->set_far(500.0f);
	camera_->look_at_from_position(Vector3(0, 5, 25), Vector3(0, 3, -5), Vector3(0, 1, 0));
	camera_->make_current();

	const std::vector<Vector3> pos = positions();
	auto make_multimesh = [&](bool p_lit) {
		Ref<QuadMesh> quad;
		quad.instantiate();
		quad->set_size(Vector2(1, 1));
		quad->set_material(ref_material(puff, p_lit, false));
		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_colors(true);
		mm->set_mesh(quad);
		mm->set_instance_count(int32_t(pos.size()));
		for (size_t i = 0; i < pos.size(); ++i) {
			mm->set_instance_transform(int32_t(i), Transform3D(Basis(), pos[i]));
			mm->set_instance_color(int32_t(i), Color(1, 1, 1, 1));
		}
		MultiMeshInstance3D *mi = memnew(MultiMeshInstance3D);
		mi->set_multimesh(mm);
		mi->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		mi->set_visible(false);
		add_child(mi);
		return mi;
	};
	ref_unlit_ = make_multimesh(false);
	ref_lit_ = make_multimesh(true);
	{
		gpu_ = memnew(Node3D);
		add_child(gpu_);
		Ref<QuadMesh> quad;
		quad.instantiate();
		quad->set_size(Vector2(1, 1));
		quad->set_material(ref_material(puff, true, true));
		for (int k = 0; k < 16; ++k) {
			Ref<ParticleProcessMaterial> pm;
			pm.instantiate();
			pm->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_BOX);
			pm->set_emission_box_extents(Vector3(5, 3.75f, 3.75f));
			pm->set_gravity(Vector3(0, 0, 0));
			pm->set_param_min(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, 0.0f);
			pm->set_param_max(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, 0.0f);
			GPUParticles3D *g = memnew(GPUParticles3D);
			g->set_amount(1000);
			g->set_lifetime(600.0);
			g->set_explosiveness_ratio(1.0f);
			g->set_process_material(pm);
			g->set_draw_pass_mesh(0, quad);
			g->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
			g->set_visibility_aabb(AABB(Vector3(-6, -5, -5), Vector3(12, 10, 10)));
			g->set_position(Vector3(-15.0f + 10.0f * float(k % 4), 4.25f, -16.25f + 7.5f * float(k / 4)));
			gpu_->add_child(g);
		}
		gpu_->set_visible(false);
	}
	{
		lights_ = memnew(Node3D);
		add_child(lights_);
		uint64_t s = 0x64;
		for (uint32_t i = 0; i < kFxRefLights; ++i) {
			OmniLight3D *l = memnew(OmniLight3D);
			l->set_param(Light3D::PARAM_RANGE, 6.0f);
			l->set_param(Light3D::PARAM_ENERGY, 1.0f);
			l->set_color(Color(1.0f, 0.6f + 0.4f * unit(s), 0.4f + 0.3f * unit(s)));
			l->set_shadow(false);
			l->set_position(Vector3(-20.0f + 40.0f * unit(s), 0.5f + 7.5f * unit(s), -20.0f + 30.0f * unit(s)));
			lights_->add_child(l);
		}
		lights_->set_visible(false);
	}
	{
		renderer_ = memnew(Bf3FxRenderer);
		add_child(renderer_);
		renderer_->set_atlas_textures(puff, flat_normal());
		renderer_->set_soft_particles(!no_soft_);
		fx::FxWorldConfig cfg;
		cfg.max_particles = 1u << 16;
		world_ = std::make_unique<fx::FxWorld>(cfg);
		const std::shared_ptr<const fx::EffectDef> def = vfx2_def();
		for (uint32_t k = 0; k < kFxRefEffects; ++k) {

			fx::EffectStart s;
			s.transform.trans = fx::Vec3{ -15.0f + 10.0f * float(k % 4), 4.25f, -16.25f + 7.5f * float(k / 4) };
			s.seed = k + 1;
			if (!world_->start(def, s).ok()) {
				log(fmt("VFX2 start %u refused", k));
				break;
			}
		}
		renderer_->step_world(*world_, 1.0 / 60.0, fx::FxView{});
		log(fmt("VFX2 world: %zu particles, %zu outputs", world_->particles().size(), world_->outputs().size()));
	}
	RenderingServer::get_singleton()->viewport_set_measure_render_time(get_viewport()->get_viewport_rid(), true);
	results_.resize(size_t(Row::Count));
	for (size_t r = 0; r < results_.size(); ++r) {
		results_[r].name = row_name(int(r));
	}
	built_ = true;
}

void Bf3FxRef::set_row(Row p_row, bool p_on) {
	ref_unlit_->set_visible(p_on && p_row == Row::RefUnlit);
	ref_lit_->set_visible(p_on && (p_row == Row::RefLitSoft || p_row == Row::RefLitSoft64));
	gpu_->set_visible(p_on && p_row == Row::Gpu16x1000);
	lights_->set_visible(p_on && (p_row == Row::RefLitSoft64 || p_row == Row::Vfx2_64));
	vfx2_drawing_ = p_on && (p_row == Row::Vfx2 || p_row == Row::Vfx2_64);
	if (!vfx2_drawing_) {
		renderer_->hide_all();
	}
}

void Bf3FxRef::_process(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint() || finished_) {
		return;
	}
	if (!built_) {
		build();
		set_row(Row(0), false);
		return;
	}
	if (busy_ms_ > 0.0 && vfx2_drawing_) {
		const uint64_t until = Time::get_singleton()->get_ticks_usec() + uint64_t(busy_ms_ * 1000.0);
		while (Time::get_singleton()->get_ticks_usec() < until) {
		}
	}
	if (vfx2_drawing_) {
		const Transform3D t = camera_->get_global_transform();
		fx::FxView v;
		v.camera_position = fx::Vec3{ t.origin.x, t.origin.y, t.origin.z };
		const Vector3 f = -t.basis.get_column(2);
		v.camera_forward = fx::Vec3{ f.x, f.y, f.z };
		if (!static_ || renderer_->stats().drawn_instances == 0) {
			renderer_->step_world(*world_, p_delta, v);
			renderer_->draw(*world_, v, empty_atlas_);
		}
		if (renderer_->stats().drawn_instances != world_->particles().size()) {
			++vfx2_mismatch_;
		}
		results_[row_index_].drawn_vfx2 = renderer_->stats().drawn_instances;
		results_[row_index_].step_ms.push_back(renderer_->stats().step_ms);
		results_[row_index_].prepare_ms.push_back(renderer_->stats().prepare_ms);
		results_[row_index_].upload_ms.push_back(renderer_->stats().upload_ms);
	}
	if (warmup_frames_ > 0) {
		--warmup_frames_;
		if (warmup_frames_ == 0) {
			log(fmt("rows: %zu, %u on / off pairs of %.1f s each (first %.1f s of a window dropped)", results_.size(), pairs_, window_, kFxRefSkipSeconds));
			set_row(Row(row_index_), true);
			window_index_ = 0;
			window_t_ = 0;
		}
		return;
	}
	const RID vp = get_viewport()->get_viewport_rid();
	const double gpu = RenderingServer::get_singleton()->viewport_get_measured_render_time_gpu(vp);
	RowResult &r = results_[row_index_];
	const bool on = (window_index_ % 2) == 0;
	if (window_t_ >= kFxRefSkipSeconds) {
		(on ? r.on_gpu : r.off_gpu).push_back(gpu);
		(on ? r.on_frame : r.off_frame).push_back(p_delta * 1000.0);
	}
	if (!shot_dir_.empty() && on && window_index_ == 0 && !r.shot && window_t_ >= 1.5) {

		r.shot = true;
		Ref<Image> img = get_viewport()->get_texture()->get_image();
		const std::string path = shot_dir_ + "/fx2_ref_" + r.name + ".png";
		if (img.is_valid() && img->save_png(to_godot(path)) == OK) {
			log("shot " + path);
		}
	}
	window_t_ += p_delta;
	if (window_t_ >= window_) {
		window_t_ = 0;
		++window_index_;
		++r.windows;
		if (window_index_ >= pairs_ * 2) {
			r.on_p50 = rounded_rank(r.on_gpu, 0.5);
			r.off_p50 = rounded_rank(r.off_gpu, 0.5);
			r.cost_gpu = r.on_p50 - r.off_p50;
			r.frame_on_p50 = rounded_rank(r.on_frame, 0.5);
			r.frame_off_p50 = rounded_rank(r.off_frame, 0.5);
			log(fmt("%-16s GPU on p50 %.3f ms, off p50 %.3f ms, cost %.3f ms; frame on p50 %.3f ms, off p50 %.3f ms (%zu / %zu samples)", r.name.c_str(), r.on_p50,
					r.off_p50, r.cost_gpu, r.frame_on_p50, r.frame_off_p50, r.on_gpu.size(), r.off_gpu.size()));
			set_row(Row(row_index_), false);
			++row_index_;
			window_index_ = 0;
			if (row_index_ >= results_.size()) {
				report_and_finish();
				return;
			}
		}
		set_row(Row(row_index_), (window_index_ % 2) == 0);
	}
}

void Bf3FxRef::report_and_finish() {
	finished_ = true;
	const RowResult &ref = results_[size_t(Row::RefLitSoft)];
	const RowResult &vfx = results_[size_t(Row::Vfx2)];
	const double ratio = ref.cost_gpu > 0.0 ? vfx.cost_gpu / ref.cost_gpu : 1e9;
	log("---- FX REF (1920 x 1080, vsync off; shared machine: GPU times are noisy) ----");
	for (const RowResult &r : results_) {
		log(fmt("%-16s cost %.3f ms GPU (on %.3f, off %.3f); frame on %.3f ms", r.name.c_str(), r.cost_gpu, r.on_p50, r.off_p50, r.frame_on_p50));
	}
	log(fmt("vfx2 drawn %u instances (mismatching frames %llu); main thread p50: step %.3f ms, prepare %.3f ms, upload "
			"%.3f ms; params uploads %llu",
			vfx.drawn_vfx2, (unsigned long long)vfx2_mismatch_, rounded_rank(vfx.step_ms, 0.5),
			rounded_rank(vfx.prepare_ms, 0.5), rounded_rank(vfx.upload_ms, 0.5),
			(unsigned long long)renderer_->stats().params_uploads));
	const bool pass = ratio <= kFxRefRatioMax && vfx.drawn_vfx2 == kFxRefParticles && vfx2_mismatch_ == 0;
	log(fmt("gate: vfx2 cost / ref-lit-soft cost = %.3f / %.3f = %.3f (<= %.2f): %s", vfx.cost_gpu, ref.cost_gpu, ratio, kFxRefRatioMax, pass ? "PASS" : "FAIL"));
	log(std::string("FX REF: ") + (pass ? "PASS" : "FAIL"));
	if (renderer_ != nullptr) {
		renderer_->clear();
	}
	get_tree()->quit(check_ ? (pass ? 0 : 1) : 0);
}

}
