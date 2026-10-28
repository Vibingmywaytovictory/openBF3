#include "level_upload.h"

#include "godot_util.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace bf3::godot_side {

using namespace godot;
namespace render = bf3::render;
using bf3::host::LoadStage;

namespace {

uint64_t now_us() {

	return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

Image::Format image_format(render::GpuFormat p_format) {
	switch (p_format) {
		case render::GpuFormat::Bc1:
			return Image::FORMAT_DXT1;
		case render::GpuFormat::Bc2:
			return Image::FORMAT_DXT3;
		case render::GpuFormat::Bc3:
			return Image::FORMAT_DXT5;
		case render::GpuFormat::Bc4:
			return Image::FORMAT_RGTC_R;
		case render::GpuFormat::Bc5:
			return Image::FORMAT_RGTC_RG;
		case render::GpuFormat::Rgba8:
			return Image::FORMAT_RGBA8;
		case render::GpuFormat::L8:
			return Image::FORMAT_L8;
		case render::GpuFormat::Rg8:
			return Image::FORMAT_RG8;
	}
	return Image::FORMAT_RGBA8;
}

void octahedron_encode(float p_x, float p_y, float p_z, float &r_u, float &r_v) {
	const float sum = std::fabs(p_x) + std::fabs(p_y) + std::fabs(p_z);
	float nx = p_x / sum, ny = p_y / sum;
	const float nz = p_z / sum;
	float ox, oy;
	if (nz >= 0.0f) {
		ox = nx;
		oy = ny;
	} else {
		ox = (1.0f - std::fabs(ny)) * (nx >= 0.0f ? 1.0f : -1.0f);
		oy = (1.0f - std::fabs(nx)) * (ny >= 0.0f ? 1.0f : -1.0f);
	}
	r_u = ox * 0.5f + 0.5f;
	r_v = oy * 0.5f + 0.5f;
}

void octahedron_tangent_encode(float p_x, float p_y, float p_z, float p_sign, float &r_u, float &r_v) {
	const float bias = 1.0f / 32767.0f;
	octahedron_encode(p_x, p_y, p_z, r_u, r_v);
	r_v = std::max(r_v, bias);
	r_v = r_v * 0.5f + 0.5f;
	r_v = p_sign >= 0.0f ? r_v : 1.0f - r_v;
}

uint16_t unorm16(float p_value) {

	const float v = p_value * 65535.0f;
	return uint16_t(v < 0.0f ? 0.0f : (v > 65535.0f ? 65535.0f : v));
}

void put16(uint8_t *p_dst, uint16_t p_a, uint16_t p_b) {
	std::memcpy(p_dst, &p_a, 2);
	std::memcpy(p_dst + 2, &p_b, 2);
}

std::string shader_code(bool p_alpha, bool p_double_sided) {
	std::string code = "shader_type spatial;\n";
	code += std::string("render_mode blend_mix, depth_draw_opaque, diffuse_burley, specular_schlick_ggx, ") + (p_double_sided ? "cull_disabled" : "cull_back") +
			";\n";
	code += R"(uniform sampler2D albedo_tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_tex : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool has_albedo = false;
uniform bool has_normal = false;
uniform vec3 albedo_tint : source_color = vec3(0.55);
uniform float roughness_value = 0.6;
void fragment() {
	vec4 c = has_albedo ? texture(albedo_tex, UV) : vec4(1.0);
	ALBEDO = c.rgb * albedo_tint;
	ROUGHNESS = roughness_value;
	METALLIC = 0.0;
	SPECULAR = 0.5;
	if (has_normal) {
		NORMAL_MAP = vec3(texture(normal_tex, UV).rg, 1.0);
	}
)";
	if (p_alpha) {
		code += "\tALPHA = c.a;\n\tALPHA_SCISSOR_THRESHOLD = 0.5;\n";
	}
	code += "}\n";
	return code;
}

std::string shader_code_l1(int p_alpha_mode, bool p_double_sided) {
	std::string code = "shader_type spatial;\n";
	code += std::string("render_mode blend_mix, ") + (p_alpha_mode == 2 ? "depth_draw_never" : "depth_draw_opaque") +
			", diffuse_burley, specular_schlick_ggx, " + (p_double_sided ? "cull_disabled" : "cull_back") + ";\n";
	code += R"(uniform sampler2D albedo_tex : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D normal_tex : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform bool has_albedo = false;
uniform bool has_normal = false;
uniform int albedo_uv = 0;
uniform float albedo_scale = 1.0;
uniform int normal_uv = 0;
uniform float normal_scale = 1.0;
uniform vec3 albedo_tint : source_color = vec3(0.55);
uniform float roughness_value = 0.6;
uniform float glass_opacity = 0.0;
varying vec3 world_pos;
varying vec3 world_nrm;
void vertex() {
	world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	world_nrm = mat3(MODEL_MATRIX) * NORMAL;
}
vec2 look_uv(int p_set, float p_scale, vec2 p_uv0, vec2 p_uv1) {
	if (p_set == 1) {
		return p_uv1 * p_scale;
	}
	if (p_set == 3) {
		vec3 n = abs(world_nrm);
		vec2 p = (n.y >= n.x && n.y >= n.z) ? world_pos.xz : (n.x >= n.z ? vec2(world_pos.z, -world_pos.y) : vec2(world_pos.x, -world_pos.y));
		return p * p_scale;
	}
	return p_uv0 * p_scale;
}
void fragment() {
	vec4 c = has_albedo ? texture(albedo_tex, look_uv(albedo_uv, albedo_scale, UV, UV2)) : vec4(1.0);
	ALBEDO = c.rgb * albedo_tint;
	ROUGHNESS = roughness_value;
	METALLIC = 0.0;
	SPECULAR = 0.5;
	if (has_normal) {
		NORMAL_MAP = vec3(texture(normal_tex, look_uv(normal_uv, normal_scale, UV, UV2)).rg, 1.0);
	}
)";
	if (p_alpha_mode == 1) {
		code += "\tALPHA = c.a;\n\tALPHA_SCISSOR_THRESHOLD = 0.5;\n";
	} else if (p_alpha_mode == 2) {
		code += "\tALPHA = glass_opacity > 0.0 ? glass_opacity : c.a;\n";
	}
	code += "}\n";
	return code;
}
}

Transform3D instance_transform(const bf3::core::level::LevelTransform &p_t) {

	const float s = p_t.scale;
	return Transform3D(Basis(p_t.right[0] * s, p_t.up[0] * s, p_t.forward[0] * s, p_t.right[1] * s, p_t.up[1] * s, p_t.forward[1] * s, p_t.right[2] * s,
							   p_t.up[2] * s, p_t.forward[2] * s),
			Vector3(p_t.trans[0], p_t.trans[1], p_t.trans[2]));
}

Dictionary encode_rs_surface(const render::PreparedSurface &p_surface, const RID &p_material) {
	RenderingServer *rs = RenderingServer::get_singleton();
	const int64_t vc = int64_t(p_surface.vertex_count());
	if (vc > (int64_t(1) << 28) || p_surface.indices.size() > (size_t(1) << 30)) {
		return Dictionary();
	}
	const int32_t vc32 = int32_t(vc);
	const bool normals = p_surface.normals.size() == size_t(vc) * 3 && vc > 0;
	const bool tangents = normals && p_surface.tangents.size() == size_t(vc) * 4;
	const bool uv0 = p_surface.uv0.size() == size_t(vc) * 2 && vc > 0;
	const bool uv1 = uv0 && p_surface.uv1.size() == size_t(vc) * 2;
	uint64_t format = RenderingServer::ARRAY_FORMAT_VERTEX | RenderingServer::ARRAY_FORMAT_INDEX | RenderingServer::ARRAY_FLAG_FORMAT_CURRENT_VERSION;
	if (normals) {
		format |= RenderingServer::ARRAY_FORMAT_NORMAL | RenderingServer::ARRAY_FORMAT_TANGENT;
	}
	if (uv0) {
		format |= RenderingServer::ARRAY_FORMAT_TEX_UV;
	}
	if (uv1) {
		format |= RenderingServer::ARRAY_FORMAT_TEX_UV2;
	}
	const BitField<RenderingServer::ArrayFormat> f = int64_t(format);
	const int64_t vstride = rs->mesh_surface_get_format_vertex_stride(f, vc32);
	const int64_t nstride = rs->mesh_surface_get_format_normal_tangent_stride(f, vc32);
	const int64_t astride = rs->mesh_surface_get_format_attribute_stride(f, vc32);
	const int64_t istride = rs->mesh_surface_get_format_index_stride(f, vc32);
	const int64_t off_normal = normals ? rs->mesh_surface_get_format_offset(f, vc32, RenderingServer::ARRAY_NORMAL) : 0;
	const int64_t off_tangent = normals ? rs->mesh_surface_get_format_offset(f, vc32, RenderingServer::ARRAY_TANGENT) : 0;
	const int64_t off_uv0 = uv0 ? rs->mesh_surface_get_format_offset(f, vc32, RenderingServer::ARRAY_TEX_UV) : 0;
	const int64_t off_uv1 = uv1 ? rs->mesh_surface_get_format_offset(f, vc32, RenderingServer::ARRAY_TEX_UV2) : 0;

	PackedByteArray vertex;
	vertex.resize(vc * (vstride + (normals ? nstride : 0)));
	uint8_t *vw = vertex.ptrw();
	Vector3 lo, hi;
	for (int64_t i = 0; i < vc; ++i) {
		const float *p = &p_surface.positions[size_t(i) * 3];
		std::memcpy(vw + i * vstride, p, 12);
		const Vector3 v(p[0], p[1], p[2]);
		if (i == 0) {
			lo = hi = v;
		} else {
			lo = Vector3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
			hi = Vector3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
		}
		if (!normals) {
			continue;
		}
		const float *n = &p_surface.normals[size_t(i) * 3];
		float u, w;
		octahedron_encode(n[0], n[1], n[2], u, w);
		put16(vw + off_normal + i * nstride, unorm16(u), unorm16(w));
		if (tangents) {
			const float *t = &p_surface.tangents[size_t(i) * 4];
			octahedron_tangent_encode(t[0], t[1], t[2], t[3], u, w);
		} else {

			const Vector3 nn = Vector3(n[0], n[1], n[2]);
			const Vector3 t = Vector3(nn.z, -nn.x, nn.y).cross(nn.normalized()).normalized();
			octahedron_tangent_encode(t.x, t.y, t.z, 1.0f, u, w);
		}
		uint16_t tu = unorm16(u), tv = unorm16(w);
		if (tu == 0 && tv == 65535) {
			tu = 65535;
		}
		put16(vw + off_tangent + i * nstride, tu, tv);
	}
	PackedByteArray attributes;
	if (astride > 0) {
		attributes.resize(vc * astride);
		uint8_t *aw = attributes.ptrw();
		for (int64_t i = 0; i < vc; ++i) {
			if (uv0) {
				std::memcpy(aw + off_uv0 + i * astride, &p_surface.uv0[size_t(i) * 2], 8);
			}
			if (uv1) {
				std::memcpy(aw + off_uv1 + i * astride, &p_surface.uv1[size_t(i) * 2], 8);
			}
		}
	}
	const int64_t ic = int64_t(p_surface.indices.size());
	PackedByteArray index;
	index.resize(ic * istride);
	uint8_t *iw = index.ptrw();
	for (int64_t i = 0; i < ic; ++i) {
		const int32_t v = p_surface.indices[size_t(i)];
		if (istride == 2) {
			const uint16_t s = uint16_t(v);
			std::memcpy(iw + i * 2, &s, 2);
		} else {
			std::memcpy(iw + i * 4, &v, 4);
		}
	}

	Dictionary d;
	d["primitive"] = int64_t(RenderingServer::PRIMITIVE_TRIANGLES);
	d["format"] = int64_t(format);
	d["vertex_data"] = vertex;
	d["vertex_count"] = vc;
	if (astride > 0) {
		d["attribute_data"] = attributes;
	}
	d["index_data"] = index;
	d["index_count"] = ic;
	d["aabb"] = AABB(lo, hi - lo);
	d["uv_scale"] = Vector4();
	if (p_material.is_valid()) {
		d["material"] = p_material;
	}
	return d;
}

LevelUpload::LevelUpload() = default;

LevelUpload::~LevelUpload() = default;

void LevelUpload::begin(std::shared_ptr<const render::LevelScenePlan> p_plan, const RID &p_scenario, bf3::host::LoadProgress *p_progress,
		const Options &p_options) {
	plan_ = std::move(p_plan);
	scenario_ = p_scenario;
	progress_ = p_progress;
	options_ = p_options;
	const render::LevelScenePlan &plan = *plan_;
	materials_.assign(plan.materials.size(), RID());
	terrain_materials_.assign(plan.terrains.size(), RID());
	textures_.assign(plan.textures.size(), RID());
	meshes_.assign(plan.meshes.size(), RID());
	terrain_meshes_.assign(plan.terrains.size(), RID());
	instances_by_mesh_.assign(plan.meshes.size(), {});
	for (size_t i = 0; i < plan.instances.size(); ++i) {
		if (plan.instances[i].mesh < plan.meshes.size()) {
			instances_by_mesh_[plan.instances[i].mesh].push_back(uint32_t(i));
		}
	}
	texture_users_.assign(plan.textures.size(), {});
	for (size_t m = 0; m < plan.materials.size(); ++m) {
		const render::SceneMaterialPlan &mp = plan.materials[m];
		if (mp.albedo >= 0 && size_t(mp.albedo) < plan.textures.size()) {
			texture_users_[size_t(mp.albedo)].push_back({ uint32_t(m), false });
		}
		if (mp.normal >= 0 && size_t(mp.normal) < plan.textures.size()) {
			texture_users_[size_t(mp.normal)].push_back({ uint32_t(m), true });
		}
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (plan.look == render::SceneLook::L1) {
		for (int v = 0; v < 6; ++v) {
			const RID shader = rs->shader_create();
			rs->shader_set_code(shader, to_godot(shader_code_l1(v / 2, (v & 1) != 0)));
			shaders_.push_back(shader);
		}
	} else {
		for (int v = 0; v < 4; ++v) {
			const RID shader = rs->shader_create();
			rs->shader_set_code(shader, to_godot(shader_code((v & 2) != 0, (v & 1) != 0)));
			shaders_.push_back(shader);
		}
	}
	if (progress_ != nullptr) {
		progress_->begin(LoadStage::Materials, uint32_t(plan.materials.size() + plan.terrains.size()));
		progress_->begin(LoadStage::Meshes, uint32_t(plan.meshes.size() + plan.terrains.size()));
		progress_->begin(LoadStage::Textures, uint32_t(plan.textures.size()));
	}
}

void LevelUpload::lanes_finished() {
	lanes_done_.store(true, std::memory_order_release);
}

void LevelUpload::push(Item &&p_item) {
	std::lock_guard lock(queue_mutex_);
	queue_.push_back(std::move(p_item));
	std::lock_guard stats_lock(stats_mutex_);
	if (stats_.first_item_us == 0) {
		stats_.first_item_us = now_us();
	}
	stats_.queued_peak = std::max<uint64_t>(stats_.queued_peak, queue_.size());
	if (queue_.back().kind == Kind::Texture) {
		stats_.texture_bytes += queue_.back().bytes;
	} else {
		stats_.mesh_bytes += queue_.back().bytes;
	}
}

void LevelUpload::mesh(render::PreparedSceneMesh &&p_mesh, uint64_t p_bytes) {
	Item item;
	item.kind = Kind::Mesh;
	item.index = p_mesh.mesh;
	item.bytes = p_bytes;
	item.mesh = std::make_unique<render::PreparedSceneMesh>(std::move(p_mesh));
	push(std::move(item));
}

void LevelUpload::texture(uint32_t p_texture, render::PreparedTexture &&p_data, uint64_t p_bytes) {
	Item item;
	item.kind = Kind::Texture;
	item.index = p_texture;
	item.bytes = p_bytes;
	item.texture = std::make_unique<render::PreparedTexture>(std::move(p_data));
	push(std::move(item));
}

void LevelUpload::terrain(uint32_t p_terrain, render::PreparedMesh &&p_mesh, uint64_t p_bytes) {
	Item item;
	item.kind = Kind::Terrain;
	item.index = p_terrain;
	item.bytes = p_bytes;
	item.terrain = std::make_unique<render::PreparedMesh>(std::move(p_mesh));
	push(std::move(item));
}

void LevelUpload::failure(const std::string &p_item, const std::string &p_error) {
	std::lock_guard lock(queue_mutex_);
	failures_.emplace_back(p_item, p_error);
	std::lock_guard stats_lock(stats_mutex_);
	++stats_.failures;
}

void LevelUpload::run_task() {
	const auto start = std::chrono::steady_clock::now();
	RenderingServer *rs = RenderingServer::get_singleton();
	const render::LevelScenePlan &plan = *plan_;
	UploadStats local;
	for (;;) {
		Item item;
		{
			std::lock_guard lock(queue_mutex_);
			if (queue_.empty()) {
				break;
			}
			item = std::move(queue_.front());
			queue_.pop_front();
		}
		Done done;
		done.kind = item.kind;
		done.index = item.index;
		if (item.kind == Kind::Texture) {
			const render::PreparedTexture &t = *item.texture;
			PackedByteArray bytes;
			bytes.resize(int64_t(t.data.size()));
			std::memcpy(bytes.ptrw(), t.data.data(), t.data.size());
			const int32_t width = int32_t(t.width), height = int32_t(t.height);
			const bool mipmaps = t.levels > 1;
			const Image::Format format = image_format(t.format);
			item.texture.reset();
			const Ref<Image> image = Image::create_from_data(width, height, mipmaps, format, bytes);
			if (image.is_valid() && !image->is_empty()) {
				done.rid = rs->texture_2d_create(image);
				++local.textures;
			}
		} else {
			const render::PreparedMesh &pm = item.kind == Kind::Mesh ? item.mesh->lod0 : *item.terrain;
			TypedArray<Dictionary> surfaces;
			for (const render::PreparedSurface &s : pm.surfaces) {
				RID material;
				if (item.kind == Kind::Terrain) {
					material = item.index < terrain_materials_.size() ? terrain_materials_[item.index] : RID();
				} else if (item.index < plan.meshes.size()) {
					const std::vector<uint32_t> &slots = plan.meshes[item.index].surface_materials;
					if (s.material_index < slots.size() && slots[s.material_index] < materials_.size()) {
						material = materials_[slots[s.material_index]];
					}
				}
				if (s.vertex_count() == 0 || s.indices.empty()) {
					continue;
				}
				const Dictionary surface = encode_rs_surface(s, material);
				if (surface.is_empty()) {
					continue;
				}
				surfaces.push_back(surface);
				++local.surfaces;
				local.vertices += s.vertex_count();
				local.triangles += s.indices.size() / 3;
			}
			if (!surfaces.is_empty()) {
				done.rid = rs->mesh_create_from_surfaces(surfaces, 0);
				if (item.kind == Kind::Mesh) {
					++local.meshes;
				} else {
					++local.terrains;
				}
			} else {
				++local.empty_meshes;
			}
		}
		if (lanes_ != nullptr) {
			lanes_->release_bytes(item.bytes);
		}
		{
			std::lock_guard lock(done_mutex_);
			done_.push_back(done);
		}
		if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= options_.task_budget_ms) {
			break;
		}
	}
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	{
		std::lock_guard lock(stats_mutex_);
		++stats_.tasks;
		stats_.c1_work_seconds += ms / 1000.0;
		stats_.c1_max_task_ms = std::max(stats_.c1_max_task_ms, ms);
		stats_.textures += local.textures;
		stats_.meshes += local.meshes;
		stats_.terrains += local.terrains;
		stats_.surfaces += local.surfaces;
		stats_.vertices += local.vertices;
		stats_.triangles += local.triangles;
		stats_.empty_meshes += local.empty_meshes;
		stats_.last_c1_us = now_us();
	}
	in_flight_.fetch_sub(1, std::memory_order_acq_rel);
}

void LevelUpload::create_materials(double p_deadline_us) {
	const render::LevelScenePlan &plan = *plan_;
	RenderingServer *rs = RenderingServer::get_singleton();
	const size_t total = plan.materials.size() + plan.terrains.size();
	while (next_material_ < total && double(now_us()) < p_deadline_us) {
		const RID m = rs->material_create();
		if (next_material_ < plan.materials.size()) {
			const render::SceneMaterialPlan &mp = plan.materials[next_material_];
			if (plan.look == render::SceneLook::L1) {

				static const bool blend_off = ("," + user_arg_value("look-off") + ",").find(",blend,") != std::string::npos;
				const int mode = mp.blend && !blend_off ? 2 : (mp.alpha_test ? 1 : 0);
				rs->material_set_shader(m, shaders_[size_t(2 * mode + (mp.double_sided ? 1 : 0))]);
				rs->material_set_param(m, "albedo_uv", int64_t(mp.albedo_uv == render::kLookUvOverlay ? render::kLookUv0 : mp.albedo_uv));
				rs->material_set_param(m, "albedo_scale", mp.albedo_scale);
				rs->material_set_param(m, "normal_uv", int64_t(mp.normal_uv == render::kLookUvOverlay ? render::kLookUv0 : mp.normal_uv));
				rs->material_set_param(m, "normal_scale", mp.normal_scale);
				if (mp.glass) {
					rs->material_set_param(m, "glass_opacity", render::kLookGlassOpacity);
				}
			} else {
				rs->material_set_shader(m, shaders_[(mp.alpha_test ? 2 : 0) + (mp.double_sided ? 1 : 0)]);
			}

			Color tint = mp.albedo >= 0 ? Color(1, 1, 1) : Color(0.55f, 0.55f, 0.55f);
			if (const std::array<float, 4> *dark = mp.slot.vector("DiffuseDarkening")) {
				tint = Color(tint.r * (*dark)[0], tint.g * (*dark)[1], tint.b * (*dark)[2]);
			}
			float roughness = 0.6f;
			if (const std::array<float, 4> *smooth = mp.slot.vector("SmoothnessRegular")) {
				roughness = std::clamp(1.0f - (*smooth)[0], 0.05f, 1.0f);
			}
			if (plan.look == render::SceneLook::L1 && mp.glass) {
				roughness = 0.1f;
				if (mp.albedo < 0) {
					tint = Color(render::kLookGlassTint, render::kLookGlassTint, render::kLookGlassTint);
				}
			}
			rs->material_set_param(m, "albedo_tint", tint);
			rs->material_set_param(m, "roughness_value", roughness);
			materials_[next_material_] = m;
		} else {
			const render::SceneTerrainPlan &tp = plan.terrains[next_material_ - plan.materials.size()];
			rs->material_set_shader(m, shaders_[0]);
			rs->material_set_param(m, "albedo_tint", Color(tp.color[0], tp.color[1], tp.color[2]));
			rs->material_set_param(m, "roughness_value", 0.9f);
			terrain_materials_[next_material_ - plan.materials.size()] = m;
		}
		++next_material_;
		std::lock_guard lock(stats_mutex_);
		++stats_.materials;
	}
	if (progress_ != nullptr) {
		const uint32_t done = progress_->done[size_t(LoadStage::Materials)].load(std::memory_order_relaxed);
		progress_->advance(LoadStage::Materials, uint32_t(next_material_) - done);
	}
	if (next_material_ >= total) {
		if (progress_ != nullptr) {
			progress_->end(LoadStage::Materials);
		}
		materials_ready_.store(true, std::memory_order_release);
	}
}

void LevelUpload::apply_done(double p_deadline_us) {
	std::vector<Done> done;
	{
		std::lock_guard lock(done_mutex_);
		done.swap(done_);
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const render::LevelScenePlan &plan = *plan_;
	for (const Done &d : done) {
		if (d.kind == Kind::Texture) {
			++textures_done_;
			if (progress_ != nullptr) {
				progress_->advance(LoadStage::Textures);
			}
			if (d.index >= textures_.size() || !d.rid.is_valid()) {
				continue;
			}
			textures_[d.index] = d.rid;
			for (const auto &[material, normal] : texture_users_[d.index]) {
				rs->material_set_param(materials_[material], normal ? "normal_tex" : "albedo_tex", d.rid);
				rs->material_set_param(materials_[material], normal ? "has_normal" : "has_albedo", true);
				std::lock_guard lock(stats_mutex_);
				++stats_.texture_bindings;
			}
		} else if (d.kind == Kind::Mesh) {
			++meshes_done_;
			if (progress_ != nullptr) {
				progress_->advance(LoadStage::Meshes);
			}
			if (d.index < meshes_.size() && d.rid.is_valid()) {
				meshes_[d.index] = d.rid;
				pending_instances_.push_back({ d.index, 0 });
			}
		} else {
			++meshes_done_;
			if (progress_ != nullptr) {
				progress_->advance(LoadStage::Meshes);
			}
			if (d.index < terrain_meshes_.size() && d.rid.is_valid()) {
				terrain_meshes_[d.index] = d.rid;
				pending_terrains_.push_back(d.index);
			}
		}
	}
	(void)p_deadline_us;
	(void)plan;
}

void LevelUpload::create_instances(double p_deadline_us) {
	RenderingServer *rs = RenderingServer::get_singleton();
	const render::LevelScenePlan &plan = *plan_;
	uint64_t made = 0;
	while (!pending_terrains_.empty()) {
		const uint32_t t = pending_terrains_.front();
		pending_terrains_.pop_front();
		const RID inst = rs->instance_create2(terrain_meshes_[t], scenario_);
		rs->instance_set_transform(inst, instance_transform(plan.terrains[t].transform));
		instances_.push_back(inst);
		++made;
	}
	while (!pending_instances_.empty() && double(now_us()) < p_deadline_us) {
		auto &[mesh, next] = pending_instances_.front();
		const std::vector<uint32_t> &list = instances_by_mesh_[mesh];

		for (int k = 0; k < 32 && next < list.size(); ++k, ++next) {
			const RID inst = rs->instance_create2(meshes_[mesh], scenario_);
			rs->instance_set_transform(inst, instance_transform(plan.instances[list[next]].transform));
			instances_.push_back(inst);
			++made;
		}
		if (next >= list.size()) {
			pending_instances_.pop_front();
		}
	}
	if (made > 0) {
		if (progress_ != nullptr) {
			if (progress_->start_us[size_t(LoadStage::Upload)].load(std::memory_order_relaxed) == 0) {
				progress_->begin(LoadStage::Upload, uint32_t(plan.instances.size() + plan.terrains.size()));
			}
			progress_->advance(LoadStage::Upload, uint32_t(made));
		}
		std::lock_guard lock(stats_mutex_);
		stats_.instances += made;
	}
}

double LevelUpload::pump(const Callable &p_task, double p_budget_ms) {
	if (plan_ == nullptr) {
		return 0.0;
	}
	const uint64_t start = now_us();
	const double deadline = double(start) + p_budget_ms * 1000.0;
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();

	for (size_t i = 0; i < task_ids_.size();) {
		if (pool->is_task_completed(task_ids_[i])) {
			pool->wait_for_task_completion(task_ids_[i]);
			task_ids_[i] = task_ids_.back();
			task_ids_.pop_back();
		} else {
			++i;
		}
	}
	if (!materials_ready_.load(std::memory_order_acquire)) {
		create_materials(deadline);
	}

	if (materials_ready_.load(std::memory_order_acquire)) {
		size_t queued = 0;
		{
			std::lock_guard lock(queue_mutex_);
			queued = queue_.size();
		}
		while (queued > 0 && in_flight_.load(std::memory_order_acquire) < options_.max_tasks) {
			in_flight_.fetch_add(1, std::memory_order_acq_rel);
			task_ids_.push_back(pool->add_task(p_task, true, "openbf3 level upload"));
			--queued;
		}
	}
	apply_done(deadline);
	create_instances(deadline);
	const uint64_t end = now_us();
	const double ms = double(end - start) / 1000.0;
	if (progress_ != nullptr) {
		if (textures_done_ >= textures_.size() && lanes_done()) {
			progress_->end(LoadStage::Textures);
		}
		if (meshes_done_ >= meshes_.size() + terrain_meshes_.size() && lanes_done()) {
			progress_->end(LoadStage::Meshes);
		}
	}
	std::lock_guard lock(stats_mutex_);
	stats_.c2_work_seconds += ms / 1000.0;
	++stats_.c2_frames;
	stats_.c2_max_frame_ms = std::max(stats_.c2_max_frame_ms, ms);
	if (ms > 0.05) {
		stats_.last_c2_us = end;
	}
	return ms;
}

bool LevelUpload::finished() const {
	if (plan_ == nullptr || !lanes_done() || !materials_ready_.load(std::memory_order_acquire) || in_flight_.load(std::memory_order_acquire) != 0 ||
			!pending_instances_.empty() || !pending_terrains_.empty() || !task_ids_.empty()) {
		return false;
	}
	{
		std::lock_guard lock(queue_mutex_);
		if (!queue_.empty()) {
			return false;
		}
	}
	std::lock_guard lock(done_mutex_);
	return done_.empty();
}

void LevelUpload::wait_tasks() {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	{

		std::lock_guard lock(queue_mutex_);
		if (lanes_ != nullptr) {
			for (const Item &item : queue_) {
				lanes_->release_bytes(item.bytes);
			}
		}
		queue_.clear();
	}
	for (const int64_t id : task_ids_) {
		pool->wait_for_task_completion(id);
	}
	task_ids_.clear();
}

void LevelUpload::free_all() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	std::vector<Done> done;
	{
		std::lock_guard lock(done_mutex_);
		done.swap(done_);
	}
	for (const Done &d : done) {
		if (d.rid.is_valid()) {
			rs->free_rid(d.rid);
		}
	}
	for (std::vector<RID> *list : { &instances_, &meshes_, &terrain_meshes_, &materials_, &terrain_materials_, &textures_, &shaders_ }) {
		for (RID &r : *list) {
			if (r.is_valid()) {
				rs->free_rid(r);
			}
			r = RID();
		}
		list->clear();
	}
	pending_instances_.clear();
	pending_terrains_.clear();
}

UploadStats LevelUpload::stats() const {
	std::lock_guard lock(stats_mutex_);
	return stats_;
}

std::vector<std::pair<std::string, std::string>> LevelUpload::failures() const {
	std::lock_guard lock(queue_mutex_);
	return failures_;
}

size_t LevelUpload::mesh_rids() const {
	size_t n = 0;
	for (const RID &r : meshes_) {
		n += r.is_valid() ? 1 : 0;
	}
	return n;
}

RID LevelUpload::mesh_rid(uint32_t p_mesh) const {
	return p_mesh < meshes_.size() ? meshes_[p_mesh] : RID();
}

RID LevelUpload::material_rid(uint32_t p_material) const {
	return p_material < materials_.size() ? materials_[p_material] : RID();
}

RID LevelUpload::texture_rid(uint32_t p_texture) const {
	return p_texture < textures_.size() ? textures_[p_texture] : RID();
}

}
