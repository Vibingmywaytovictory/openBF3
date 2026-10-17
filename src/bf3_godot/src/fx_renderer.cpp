// Bf3FxRenderer: VFX2's Godot side (docs/design/m2a-fx2.md). See fx_renderer.h.
#include "fx_renderer.h"

#include "godot_util.h"

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace bf3::godot_side {

using namespace godot;
namespace fx = bf3::fx;

namespace {

double ms_since(std::chrono::steady_clock::time_point p_t0) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p_t0).count();
}

// The batch's position in RenderPrep's draw order (pass, then alpha / premultiplied / additive, then class).
int draw_position(const fx::FxBatchKey &p_key) {
	int blend = 0;
	switch (p_key.blend) {
		case fx::FxBlend::Alpha:
			blend = 0;
			break;
		case fx::FxBlend::Premultiplied:
			blend = 1;
			break;
		case fx::FxBlend::Additive:
		default:
			blend = 2;
			break;
	}
	return (int(p_key.pass) * int(fx::kFxBlendCount) + blend) * int(fx::kFxShaderClassCount) + int(p_key.shader);
}

const char *shader_path(fx::FxBlend p_blend) {
	switch (p_blend) {
		case fx::FxBlend::Additive:
			return "res://shaders/fx_additive.gdshader";
		case fx::FxBlend::Premultiplied:
			return "res://shaders/fx_premultiplied.gdshader";
		case fx::FxBlend::Alpha:
		default:
			return "res://shaders/fx_alpha.gdshader";
	}
}

Ref<ImageTexture> make_atlas_texture(const fx::AtlasImage &p_image, Image::Format p_format, std::string *r_error) {
	PackedByteArray bytes;
	bytes.resize(int64_t(p_image.bytes.size()));
	std::memcpy(bytes.ptrw(), p_image.bytes.data(), p_image.bytes.size());
	const Ref<Image> image = Image::create_from_data(int32_t(p_image.width), int32_t(p_image.height), p_image.mips.size() > 1, p_format, bytes);
	if (image.is_null() || image->is_empty()) {
		*r_error = "atlas " + p_image.partition + ": Image::create_from_data refused the prepared chain";
		return Ref<ImageTexture>();
	}
	if (size_t(image->get_mipmap_count() + 1) != p_image.mips.size()) {
		*r_error = "atlas " + p_image.partition + ": " + std::to_string(image->get_mipmap_count() + 1) + " levels, prepared " + std::to_string(p_image.mips.size());
		return Ref<ImageTexture>();
	}
	return ImageTexture::create_from_image(image);
}

uint32_t grow(uint32_t p_needed) {
	uint32_t c = kFxMinCapacity;
	while (c < p_needed && c < (1u << 30)) {
		c <<= 1;
	}
	return c;
}

} // namespace

Bf3FxRenderer::Bf3FxRenderer() = default;

Bf3FxRenderer::~Bf3FxRenderer() {
	clear();
}

void Bf3FxRenderer::_bind_methods() {}

void Bf3FxRenderer::_notification(int p_what) {
	if (p_what == NOTIFICATION_PREDELETE || p_what == NOTIFICATION_EXIT_TREE) {
		clear();
	}
}

bool Bf3FxRenderer::upload_atlas(const fx::AtlasPixels &p_pixels, std::string *r_error) {
	if (p_pixels.base.format != core::res::PixelFormat::Bc3 || p_pixels.normal.format != core::res::PixelFormat::Bc5) {
		*r_error = "atlas pixels: unexpected formats";
		return false;
	}
	// Godot's DXT5 is sampled as sRGB through the shader's source_color hint; BC5 = RGTC_RG (linear).
	Ref<ImageTexture> base = make_atlas_texture(p_pixels.base, Image::FORMAT_DXT5, r_error);
	if (base.is_null()) {
		return false;
	}
	Ref<ImageTexture> normal = make_atlas_texture(p_pixels.normal, Image::FORMAT_RGTC_RG, r_error);
	if (normal.is_null()) {
		return false;
	}
	stats_.atlas_mips_base = uint32_t(p_pixels.base.mips.size());
	stats_.atlas_mips_normal = uint32_t(p_pixels.normal.mips.size());
	stats_.atlas_ok = true;
	set_atlas_textures(base, normal);
	return true;
}

void Bf3FxRenderer::set_atlas_textures(const Ref<Texture2D> &p_base, const Ref<Texture2D> &p_normal) {
	base_atlas_ = p_base;
	normal_atlas_ = p_normal;
	for (Batch &b : batches_) {
		if (b.material.is_valid()) {
			b.material->set_shader_parameter("base_atlas", base_atlas_);
			b.material->set_shader_parameter("normal_atlas", normal_atlas_);
		}
	}
}

void Bf3FxRenderer::set_soft_particles(bool p_enabled) {
	soft_ = p_enabled;
	for (Batch &b : batches_) {
		if (b.material.is_valid()) {
			b.material->set_shader_parameter("soft_enabled", soft_);
		}
	}
}

void Bf3FxRenderer::set_debug_view(int p_mode) {
	debug_view_ = p_mode;
	for (Batch &b : batches_) {
		if (b.material.is_valid()) {
			b.material->set_shader_parameter("debug_view", debug_view_);
		b.material->set_shader_parameter("self_lit_scale", kFxSelfLitScale);
		b.material->set_shader_parameter("emissive_scale", kFxEmissiveScale);
		}
	}
}

void Bf3FxRenderer::set_first_person_projection(float p_fov_override, float p_z_clip_scale) {
	fp_fov_ = p_fov_override;
	fp_z_clip_ = p_z_clip_scale;
	for (Batch &b : batches_) {
		if (b.material.is_valid()) {
			b.material->set_shader_parameter("fp_fov_override", fp_fov_);
			b.material->set_shader_parameter("fp_z_clip_scale", fp_z_clip_);
		}
	}
}

bool Bf3FxRenderer::ensure_resources() {
	if (resources_) {
		return true;
	}
	if (!is_inside_tree() || get_world_3d().is_null()) {
		return false;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	scenario_ = get_world_3d()->get_scenario();
	Ref<QuadMesh> quad;
	quad.instantiate();
	quad->set_size(Vector2(1, 1)); // -0.5 .. 0.5, normal +Z
	quad_mesh_ = quad;
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1, 1, 1));
	box_mesh_ = box;
	std::array<Ref<Shader>, fx::kFxBlendCount> shaders;
	for (size_t b = 0; b < fx::kFxBlendCount; ++b) {
		shaders[b] = ResourceLoader::get_singleton()->load(shader_path(fx::FxBlend(b)));
		if (shaders[b].is_null()) {
			UtilityFunctions::push_error(String("[fx] shader missing: ") + shader_path(fx::FxBlend(b)));
			return false;
		}
	}
	params_lines_ = 1;
	params_staging_.assign(size_t(kFxParamsRowsPerLine) * 16, 0.0f);
	params_uploaded_ = params_staging_;
	PackedByteArray zeros;
	zeros.resize(int64_t(params_staging_.size() * sizeof(float)));
	std::memset(zeros.ptrw(), 0, size_t(zeros.size()));
	params_tex_ = ImageTexture::create_from_image(Image::create_from_data(kFxParamsRowsPerLine * 4, 1, false, Image::FORMAT_RGBAF, zeros));
	for (size_t i = 0; i < fx::kMaxFxBatches; ++i) {
		const fx::FxBatchKey key = fx::fx_batch_key_at(i);
		Batch &b = batches_[i];
		b.material.instantiate();
		b.material->set_shader(shaders[size_t(key.blend)]);
		b.material->set_render_priority(draw_position(key) - 64);
		b.material->set_shader_parameter("shader_class", int(key.shader));
		b.material->set_shader_parameter("first_person", key.pass == fx::FxPass::FirstPerson);
		b.material->set_shader_parameter("fp_fov_override", fp_fov_);
		b.material->set_shader_parameter("fp_z_clip_scale", fp_z_clip_);
		b.material->set_shader_parameter("soft_enabled", soft_);
		b.material->set_shader_parameter("debug_view", debug_view_);
		b.material->set_shader_parameter("self_lit_scale", kFxSelfLitScale);
		b.material->set_shader_parameter("emissive_scale", kFxEmissiveScale);
		b.material->set_shader_parameter("base_atlas", base_atlas_);
		b.material->set_shader_parameter("normal_atlas", normal_atlas_);
		b.material->set_shader_parameter("fx_params", params_tex_);
		b.multimesh = rs->multimesh_create();
		rs->multimesh_set_mesh(b.multimesh, (key.shader == fx::FxShaderClass::Mesh ? box_mesh_ : quad_mesh_)->get_rid());
		b.capacity = kFxMinCapacity;
		rs->multimesh_allocate_data(b.multimesh, int32_t(b.capacity), RenderingServer::MULTIMESH_TRANSFORM_3D, true, true);
		rs->multimesh_set_visible_instances(b.multimesh, 0);
		b.buffer.resize(int64_t(b.capacity) * 20);
		b.instance = rs->instance_create2(b.multimesh, scenario_);
		rs->instance_geometry_set_material_override(b.instance, b.material->get_rid());
		rs->instance_geometry_set_cast_shadows_setting(b.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
		rs->instance_set_visible(b.instance, false);
		b.shown = false;
		b.visible = 0;
	}
	for (Light &l : lights_) {
		l.light = rs->omni_light_create();
		rs->light_set_shadow(l.light, false);
		rs->light_set_param(l.light, RenderingServer::LIGHT_PARAM_SPECULAR, 0.5f);
		l.instance = rs->instance_create2(l.light, scenario_);
		rs->instance_set_visible(l.instance, false);
		l.shown = false;
	}
	resources_ = true;
	return true;
}

void Bf3FxRenderer::clear() {
	if (!resources_) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (Batch &b : batches_) {
			if (b.instance.is_valid()) {
				rs->free_rid(b.instance);
			}
			if (b.multimesh.is_valid()) {
				rs->free_rid(b.multimesh);
			}
		}
		for (Light &l : lights_) {
			if (l.instance.is_valid()) {
				rs->free_rid(l.instance);
			}
			if (l.light.is_valid()) {
				rs->free_rid(l.light);
			}
		}
	}
	batches_ = {};
	lights_ = {};
	quad_mesh_.unref();
	box_mesh_.unref();
	params_tex_.unref();
	resources_ = false;
}

void Bf3FxRenderer::run_chunk(uint32_t p_index) {
	const size_t first = size_t(p_index) * chunk_size_;
	if (first < chunk_items_) {
		chunk_world_->run_items(first, std::min(chunk_size_, chunk_items_ - first));
	}
}

void Bf3FxRenderer::step_world(fx::FxWorld &r_world, double p_dt, const fx::FxView &p_view) {
	const auto t0 = std::chrono::steady_clock::now();
	const uint32_t k = r_world.begin_frame(float(p_dt), p_view);
	for (uint32_t s = 0; s < k; ++s) {
		const size_t n = r_world.begin_step();
		if (n < kFxSerialItems) {
			r_world.run_items(0, n);
		} else {
			const int32_t tasks = int32_t(std::min<size_t>(size_t(kFxStepTasks), n));
			chunk_world_ = &r_world;
			chunk_items_ = n;
			chunk_size_ = (n + size_t(tasks) - 1) / size_t(tasks);
			WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
			const int64_t group = pool->add_group_task(callable_mp(this, &Bf3FxRenderer::run_chunk), tasks, tasks, true, "fx step");
			pool->wait_for_group_task_completion(group);
			chunk_world_ = nullptr;
		}
		r_world.end_step();
	}
	stats_.step_ms = ms_since(t0);
}

void Bf3FxRenderer::upload_params() {
	const std::span<const fx::FxEmitterParams> rows = prep_.emitter_params();
	const int32_t lines = std::max<int32_t>(1, int32_t((rows.size() + kFxParamsRowsPerLine - 1) / kFxParamsRowsPerLine));
	int32_t cap_lines = 1;
	while (cap_lines < lines) {
		cap_lines <<= 1;
	}
	const size_t floats = size_t(cap_lines) * kFxParamsRowsPerLine * 16;
	params_staging_.assign(floats, 0.0f);
	if (!rows.empty()) {
		std::memcpy(params_staging_.data(), rows.data(), rows.size_bytes()); // rows are contiguous: 4 texels each
	}
	if (cap_lines == params_lines_ && params_staging_ == params_uploaded_) {
		return;
	}
	PackedByteArray bytes;
	bytes.resize(int64_t(floats * sizeof(float)));
	std::memcpy(bytes.ptrw(), params_staging_.data(), floats * sizeof(float));
	const Ref<Image> image = Image::create_from_data(kFxParamsRowsPerLine * 4, cap_lines, false, Image::FORMAT_RGBAF, bytes);
	if (cap_lines == params_lines_ && params_tex_.is_valid()) {
		params_tex_->update(image);
	} else {
		params_tex_ = ImageTexture::create_from_image(image);
		for (Batch &b : batches_) {
			b.material->set_shader_parameter("fx_params", params_tex_);
		}
		params_lines_ = cap_lines;
	}
	params_uploaded_ = params_staging_;
	++stats_.params_uploads;
}

void Bf3FxRenderer::draw(const fx::FxWorld &p_world, const fx::FxView &p_view, const fx::EmitterAtlas &p_atlas) {
	if (!ensure_resources()) {
		return;
	}
	auto t0 = std::chrono::steady_clock::now();
	prep_.prepare(p_world, p_view, p_atlas);
	stats_.prepare_ms = ms_since(t0);
	t0 = std::chrono::steady_clock::now();
	RenderingServer *rs = RenderingServer::get_singleton();
	upload_params();
	std::array<const fx::FxBatch *, fx::kMaxFxBatches> by_index{};
	for (const fx::FxBatch &b : prep_.batches()) {
		by_index[fx::fx_batch_index(b.key)] = &b;
	}
	const std::span<const fx::FxInstance> inst = prep_.instances();
	uint32_t drawn = 0, capacity = 0;
	for (size_t i = 0; i < fx::kMaxFxBatches; ++i) {
		Batch &b = batches_[i];
		const fx::FxBatch *src = by_index[i];
		if (src == nullptr || src->instance_count == 0) {
			if (b.shown) {
				rs->instance_set_visible(b.instance, false);
				rs->multimesh_set_visible_instances(b.multimesh, 0);
				b.shown = false;
				b.visible = 0;
			}
			capacity += b.capacity;
			continue;
		}
		const uint32_t n = src->instance_count;
		if (n > b.capacity) {
			b.capacity = grow(n);
			rs->multimesh_allocate_data(b.multimesh, int32_t(b.capacity), RenderingServer::MULTIMESH_TRANSFORM_3D, true, true);
			b.buffer.resize(int64_t(b.capacity) * 20);
			++stats_.reallocations;
		}
		static_assert(sizeof(fx::FxInstance) == 20 * sizeof(float));
		std::memcpy(b.buffer.ptrw(), inst.data() + src->first_instance, size_t(n) * sizeof(fx::FxInstance));
		rs->multimesh_set_buffer(b.multimesh, b.buffer);
		rs->multimesh_set_visible_instances(b.multimesh, int32_t(n));
		const Vector3 lo(src->aabb_min.x, src->aabb_min.y, src->aabb_min.z);
		const Vector3 hi(src->aabb_max.x, src->aabb_max.y, src->aabb_max.z);
		rs->multimesh_set_custom_aabb(b.multimesh, AABB(lo, (hi - lo).max(Vector3(0.01f, 0.01f, 0.01f))));
		if (!b.shown) {
			rs->instance_set_visible(b.instance, true);
			b.shown = true;
		}
		b.visible = n;
		drawn += n;
		capacity += b.capacity;
	}
	const std::span<const fx::FxLight> lights = prep_.lights();
	for (size_t i = 0; i < fx::kMaxFxLights; ++i) {
		Light &l = lights_[i];
		if (i >= lights.size()) {
			if (l.shown) {
				rs->instance_set_visible(l.instance, false);
				l.shown = false;
			}
			continue;
		}
		const fx::FxLight &f = lights[i];
		const float peak = std::max(f.color.x, std::max(f.color.y, f.color.z));
		const Color c = peak > 0.0f ? Color(f.color.x / peak, f.color.y / peak, f.color.z / peak) : Color(1, 1, 1);
		rs->light_set_color(l.light, c);
		rs->light_set_param(l.light, RenderingServer::LIGHT_PARAM_ENERGY, std::max(0.0f, f.intensity * peak * kFxLightEnergyScale));
		rs->light_set_param(l.light, RenderingServer::LIGHT_PARAM_RANGE, std::max(0.05f, f.radius));
		rs->instance_set_transform(l.instance, Transform3D(Basis(), Vector3(f.position.x, f.position.y, f.position.z)));
		if (!l.shown) {
			rs->instance_set_visible(l.instance, true);
			l.shown = true;
		}
	}
	stats_.upload_ms = ms_since(t0);
	stats_.drawn_instances = drawn;
	stats_.batches = uint32_t(prep_.batches().size());
	stats_.lights = uint32_t(lights.size());
	stats_.capacity = capacity;
}

void Bf3FxRenderer::hide_all() {
	if (!resources_) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	for (Batch &b : batches_) {
		if (b.shown) {
			rs->instance_set_visible(b.instance, false);
			b.shown = false;
		}
		b.visible = 0;
	}
	for (Light &l : lights_) {
		if (l.shown) {
			rs->instance_set_visible(l.instance, false);
			l.shown = false;
		}
	}
	stats_.drawn_instances = 0;
	stats_.lights = 0;
}

} // namespace bf3::godot_side
