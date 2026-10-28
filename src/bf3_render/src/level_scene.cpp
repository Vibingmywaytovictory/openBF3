#include "bf3/render/level_scene.h"

#include "level_scene_internal.h"

#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <climits>
#include <cstring>
#include <iterator>
#include <map>
#include <span>
#include <unordered_map>

namespace bf3::render {

using core::EbxInstance;
using core::EbxResolver;
using core::EbxValue;
using core::level::LevelTransform;
using core::level::MeshCensus;
using core::level::ObjectKind;
using core::level::PlacedObject;
using core::level::SceneMesh;
using core::level::World;

namespace {

using Clock = std::chrono::steady_clock;
using scene_internal::Mat3;
using Vec3 = core::Vec3;

constexpr std::string_view kUndrawnShaders[5] = { "invisible", "occluder", "shadowmesh", "lightcone_particles",
	"ui_menu" };

constexpr std::string_view kAlphaTestWords[] = { "foliage", "vegetation", "alphatest", "alpha_test", "fence", "decal", "grass", "leaf", "leaves",
	"tree", "bush", "ivy", "hair", "cutout", "masked" };

constexpr std::string_view kAlbedoParameters[] = { "Diffuse", "MainDiffuse", "TileDiffuse", "DiffuseLeaves", "DiffuseBark", "Diffuse2" };
constexpr std::string_view kNormalParameters[] = { "Normal", "MainNormal", "TileNormal" };
constexpr std::string_view kDoubleSidedWords[] = { "foliage", "vegetation", "grass", "leaf", "leaves", "tree", "bush", "ivy", "twosided", "two_sided",
	"doublesided" };

constexpr float kTerrainMargin = 256.0f;

constexpr float kSunEnergyPerColor = 0.25f;
constexpr size_t kMaxTerrainChunks = 1u << 16;

using bf3::core::iequals_ascii;
using bf3::core::seconds_since;
using bf3::core::to_lower_ascii;

bool contains_any(std::string_view p_text, std::span<const std::string_view> p_words) {
	for (std::string_view w : p_words) {
		if (p_text.find(w) != std::string_view::npos) {
			return true;
		}
	}
	return false;
}

float dot(const Vec3 &p_a, const Vec3 &p_b) {
	return p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2];
}

Vec3 cross(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a[1] * p_b[2] - p_a[2] * p_b[1], p_a[2] * p_b[0] - p_a[0] * p_b[2], p_a[0] * p_b[1] - p_a[1] * p_b[0] };
}

Vec3 normalized(const Vec3 &p_v) {
	const float len = std::sqrt(dot(p_v, p_v));
	if (!(len > 1e-20f)) {
		return p_v;
	}
	return { p_v[0] / len, p_v[1] / len, p_v[2] / len };
}

Vec3 mul(const Mat3 &p_m, const Vec3 &p_v) {
	return { dot(p_m[0], p_v), dot(p_m[1], p_v), dot(p_m[2], p_v) };
}

float det3(const Mat3 &p_m) {
	return dot(p_m[0], cross(p_m[1], p_m[2]));
}

Mat3 normal_matrix(const Mat3 &p_m) {
	const float sign = det3(p_m) < 0.0f ? -1.0f : 1.0f;
	Mat3 c = { cross(p_m[1], p_m[2]), cross(p_m[2], p_m[0]), cross(p_m[0], p_m[1]) };

	for (Vec3 &row : c) {
		for (float &v : row) {
			v *= sign;
		}
	}
	return c;
}

void recompute_aabb(PreparedSurface &r_surface) {
	const size_t n = r_surface.vertex_count();
	r_surface.aabb_min = { 0.0f, 0.0f, 0.0f };
	r_surface.aabb_max = { 0.0f, 0.0f, 0.0f };
	for (size_t i = 0; i < n; ++i) {
		for (size_t a = 0; a < 3; ++a) {
			const float v = r_surface.positions[i * 3 + a];
			if (i == 0 || v < r_surface.aabb_min[a]) {
				r_surface.aabb_min[a] = v;
			}
			if (i == 0 || v > r_surface.aabb_max[a]) {
				r_surface.aabb_max[a] = v;
			}
		}
	}
}

core::Status check_surface(const PreparedSurface &p_s, const std::string &p_mesh) {
	const size_t n = p_s.vertex_count();
	const auto bad = [&](const char *p_what) { return core::make_error("mesh " + p_mesh + " subset " + std::to_string(p_s.subset) + ": " + p_what); };
	if (p_s.positions.size() != n * 3) {
		return bad("positions are not xyz triples");
	}
	const auto sized = [&](size_t p_size, size_t p_width) { return p_size == 0 || p_size == n * p_width; };
	if (!sized(p_s.normals.size(), 3) || !sized(p_s.tangents.size(), 4) || !sized(p_s.uv0.size(), 2) || !sized(p_s.uv1.size(), 2) ||
			!sized(p_s.colors.size(), 4) || !sized(p_s.bones.size(), 4) || !sized(p_s.weights.size(), 4) || !sized(p_s.parts.size(), 1)) {
		return bad("vertex arrays of different lengths");
	}
	if (p_s.indices.size() % 3 != 0) {
		return bad("index count is not a multiple of 3");
	}
	for (int32_t i : p_s.indices) {
		if (i < 0 || size_t(i) >= n) {
			return bad("index out of range");
		}
	}
	return core::Status::success();
}

void compact_vertices(PreparedSurface &r_s, const std::vector<uint8_t> &p_keep) {
	const size_t n = r_s.vertex_count();
	std::vector<int32_t> remap(n, -1);
	int32_t next = 0;
	for (size_t i = 0; i < n; ++i) {
		if (p_keep[i] != 0) {
			remap[i] = next++;
		}
	}
	const auto squeeze = [&](auto &r_array, size_t p_width) {
		if (r_array.empty()) {
			return;
		}
		size_t out = 0;
		for (size_t i = 0; i < n; ++i) {
			if (p_keep[i] == 0) {
				continue;
			}
			for (size_t k = 0; k < p_width; ++k) {
				r_array[out * p_width + k] = r_array[i * p_width + k];
			}
			++out;
		}
		r_array.resize(out * p_width);
		r_array.shrink_to_fit();
	};
	squeeze(r_s.positions, 3);
	squeeze(r_s.normals, 3);
	squeeze(r_s.tangents, 4);
	squeeze(r_s.uv0, 2);
	squeeze(r_s.uv1, 2);
	squeeze(r_s.colors, 4);
	squeeze(r_s.bones, 4);
	squeeze(r_s.weights, 4);
	squeeze(r_s.parts, 1);
	for (int32_t &i : r_s.indices) {
		i = remap[size_t(i)];
	}
}

std::optional<Vec3> vec3_field(const EbxInstance &p_instance, std::string_view p_field) {
	const std::optional<EbxValue> v = p_instance.get(p_field);
	if (!v) {
		return std::nullopt;
	}
	Vec3 out{};
	const char *axes[3] = { "x", "y", "z" };
	for (int a = 0; a < 3; ++a) {
		const std::optional<EbxValue> c = v->get(axes[a]);
		const std::optional<float> f = c ? c->as_f32() : std::nullopt;
		if (!f || !std::isfinite(*f)) {
			return std::nullopt;
		}
		out[size_t(a)] = *f;
	}
	return out;
}

std::optional<float> float_field(const EbxInstance &p_instance, std::string_view p_field) {
	const std::optional<EbxValue> v = p_instance.get(p_field);
	const std::optional<float> f = v ? v->as_f32() : std::nullopt;
	if (!f || !std::isfinite(*f)) {
		return std::nullopt;
	}
	return f;
}

bool bool_field(const EbxInstance &p_instance, std::string_view p_field, bool p_default) {
	const std::optional<EbxValue> v = p_instance.get(p_field);
	const std::optional<bool> b = v ? v->as_bool() : std::nullopt;
	return b.value_or(p_default);
}

float max_component(const Vec3 &p_v) {
	return std::fmax(std::fmax(p_v[0], p_v[1]), p_v[2]);
}

Vec3 with_peak(const Vec3 &p_v, float p_peak) {
	const float m = max_component(p_v);
	if (!(m > 1e-6f)) {
		return p_v;
	}
	return { std::fmax(p_v[0], 0.0f) * p_peak / m, std::fmax(p_v[1], 0.0f) * p_peak / m, std::fmax(p_v[2], 0.0f) * p_peak / m };
}

SceneEnvironment read_environment(const EbxResolver &p_db, const World &p_world) {
	SceneEnvironment env;
	struct Candidate {
		EbxInstance outdoor;
		std::optional<EbxInstance> sky, fog, enlighten;
		int64_t priority = 0;
	};
	std::optional<Candidate> best;
	for (const PlacedObject &object : p_world.objects) {
		if (object.kind != ObjectKind::VisualEnvironment) {
			continue;
		}
		const EbxInstance &entity = object.entity;
		if (!bool_field(entity, "Enabled", true)) {
			continue;
		}
		Candidate c;
		bool outdoor = false;
		size_t seen = 0;
		for (const EbxInstance &component : p_db.follow_all(entity.data(), "Components")) {
			if (++seen > 256) {
				break;
			}
			if (!bool_field(component, "Enable", true)) {
				continue;
			}
			if (component.is_a("OutdoorLightComponentData") && !outdoor) {
				c.outdoor = component;
				outdoor = true;
			} else if (component.is_a("SkyComponentData") && !c.sky) {
				c.sky = component;
			} else if (component.is_a("FogComponentData") && !c.fog) {
				c.fog = component;
			} else if (component.is_a("EnlightenComponentData") && !c.enlighten) {
				c.enlighten = component;
			}
		}
		if (!outdoor) {
			continue;
		}
		const std::optional<EbxValue> priority = entity.get("Priority");
		c.priority = priority ? priority->as_i64().value_or(0) : 0;
		if (!best || c.priority > best->priority) {
			best = c;
		}
	}
	if (!best) {
		return env;
	}
	env.from_data = true;

	const float azimuth = float_field(best->outdoor, "SunRotationX").value_or(200.0f) * 3.14159265358979f / 180.0f;
	const float elevation = float_field(best->outdoor, "SunRotationY").value_or(35.0f) * 3.14159265358979f / 180.0f;
	const Vec3 to_sun = { std::sin(azimuth) * std::cos(elevation), std::sin(elevation), std::cos(azimuth) * std::cos(elevation) };
	env.sun_direction = normalized({ -to_sun[0], -to_sun[1], -to_sun[2] });
	if (const std::optional<Vec3> sun = vec3_field(best->outdoor, "SunColor"); sun && max_component(*sun) > 1e-6f) {
		env.sun_color = with_peak(*sun, 1.0f);
		env.sun_energy = std::clamp(max_component(*sun) * kSunEnergyPerColor, 0.0f, 16.0f);
	}

	std::optional<Vec3> sky, ground;
	if (best->enlighten) {
		sky = vec3_field(*best->enlighten, "SkyBoxSkyColor");
		ground = vec3_field(*best->enlighten, "SkyBoxGroundColor");
	}
	if (!sky || !(max_component(*sky) > 1e-6f)) {
		sky = vec3_field(best->outdoor, "SkyColor");
	}
	if (!ground || !(max_component(*ground) > 1e-6f)) {
		ground = vec3_field(best->outdoor, "GroundColor");
	}
	if (sky && max_component(*sky) > 1e-6f) {
		env.sky_color = with_peak(*sky, 0.7f);
	}
	if (ground && max_component(*ground) > 1e-6f) {
		env.ground_color = with_peak(*ground, 0.3f);
	}
	if (best->fog) {
		const std::optional<float> start = float_field(*best->fog, "Start");
		const std::optional<float> end = float_field(*best->fog, "End");
		if (start && end && *end > *start && *start >= 0.0f) {
			env.fog_start = *start;
			env.fog_end = *end;
			if (const std::optional<Vec3> color = vec3_field(*best->fog, "FogColor")) {
				env.fog_color = max_component(*color) > 1.0f ? with_peak(*color, 1.0f) : *color;
			}
		}
	}
	return env;
}

std::string material_key(const MaterialSlot &p_slot) {
	std::string key = p_slot.shader;
	key += '\x01';
	key += p_slot.variation_shader;
	key += '\x01';
	for (const auto &[name, texture] : p_slot.textures) {
		key += to_lower_ascii(name);
		key += '=';
		key += texture;
		key += '\x02';
	}
	key += '\x01';
	for (const auto &[name, value] : p_slot.vectors) {
		key += to_lower_ascii(name);
		key += '=';
		char bits[16];
		std::memcpy(bits, value.data(), 16);
		key.append(bits, 16);
	}
	return key;
}

std::string look_key(const LookMaterial &p_look) {
	std::string key;
	for (const std::optional<LookTexture> *t : { &p_look.color, &p_look.normal }) {
		key += '\x03';
		if (*t) {
			key += (*t)->name;
			key += char('0' + (*t)->channel);
			char bits[4];
			std::memcpy(bits, &(*t)->scale, 4);
			key.append(bits, 4);
		}
	}
	key += char('0' + int(p_look.blend));
	key += p_look.double_sided ? 'd' : 's';
	key += p_look.glass ? 'g' : '-';
	return key;
}

struct PlanBuilder {
	LevelScenePlan &plan;
	std::unordered_map<std::string, uint32_t> material_by_key;
	std::unordered_map<std::string, uint32_t> texture_by_name;
	LookResolver *resolver = nullptr;

	uint32_t look_material(const MaterialSlot &p_slot, std::string_view p_mesh) {
		LookMaterial look = resolver->resolve(p_slot, p_mesh);
		if (!look.drawn) {
			return kSceneUndrawnMaterial;
		}
		const std::string key = material_key(p_slot) + '\x04' + look_key(look);
		const auto found = material_by_key.find(key);
		if (found != material_by_key.end()) {
			return found->second;
		}
		SceneMaterialPlan m;
		m.slot = p_slot;
		m.textures.assign(p_slot.textures.size(), -1);
		m.alpha_test = look.blend == LookBlend::AlphaTest;
		m.blend = look.blend == LookBlend::Blend;
		m.double_sided = look.double_sided;
		m.glass = look.glass;
		const auto bind = [&](const LookTexture &p_t, TextureRole p_role) {
			const int32_t at = int32_t(texture(p_t.name, p_role));
			for (size_t i = 0; i < p_slot.textures.size(); ++i) {
				if (!p_t.parameter.empty() && p_slot.textures[i].first == p_t.parameter &&
						to_lower_ascii(p_slot.textures[i].second) == p_t.name) {
					m.textures[i] = at;
				}
			}
			return at;
		};
		if (look.color) {
			m.albedo = bind(*look.color, TextureRole::Color);
			m.albedo_uv = look.color->channel;
			m.albedo_scale = look.color->scale;
		}
		if (look.normal) {
			m.normal = bind(*look.normal, TextureRole::Normal);
			m.normal_uv = look.normal->channel;
			m.normal_scale = look.normal->scale;
		}
		m.look = std::move(look);
		const uint32_t at = uint32_t(plan.materials.size());
		plan.materials.push_back(std::move(m));
		material_by_key.emplace(key, at);
		return at;
	}

	uint32_t texture(const std::string &p_name, TextureRole p_role) {
		const auto found = texture_by_name.find(p_name);
		if (found != texture_by_name.end()) {
			SceneTexturePlan &t = plan.textures[found->second];
			t.role = scene_internal::merge_role(t.role, p_role);
			return found->second;
		}
		const uint32_t at = uint32_t(plan.textures.size());
		plan.textures.push_back(SceneTexturePlan{ p_name, p_role });
		texture_by_name.emplace(p_name, at);
		return at;
	}

	uint32_t material(const MaterialSlot &p_slot) {
		const std::string key = material_key(p_slot);
		const auto found = material_by_key.find(key);
		if (found != material_by_key.end()) {
			return found->second;
		}
		SceneMaterialPlan m;
		m.slot = p_slot;
		const scene_internal::ShaderFlags flags = scene_internal::shader_flags(p_slot.shader);
		m.alpha_test = flags.alpha_test;
		m.double_sided = flags.double_sided;
		m.textures.assign(p_slot.textures.size(), -1);
		const auto [albedo, normal] = scene_internal::drawn_parameters(p_slot);
		if (albedo >= 0) {
			m.albedo = m.textures[size_t(albedo)] = int32_t(texture(p_slot.textures[size_t(albedo)].second, TextureRole::Color));
		}
		if (normal >= 0) {
			m.normal = m.textures[size_t(normal)] = int32_t(texture(p_slot.textures[size_t(normal)].second, TextureRole::Normal));
		}
		const uint32_t at = uint32_t(plan.materials.size());
		plan.materials.push_back(std::move(m));
		material_by_key.emplace(key, at);
		return at;
	}
};

struct ChunkKey {
	std::array<uint8_t, 16> bytes;
	bool operator<(const ChunkKey &p_other) const { return bytes < p_other.bytes; }
};

}

namespace scene_internal {

ParameterUse parameter_use(std::string_view p_parameter) {
	for (size_t i = 0; i < std::size(kAlbedoParameters); ++i) {
		if (iequals_ascii(p_parameter, kAlbedoParameters[i])) {
			return { TextureRole::Color, int(i) };
		}
	}
	for (size_t i = 0; i < std::size(kNormalParameters); ++i) {
		if (iequals_ascii(p_parameter, kNormalParameters[i])) {
			return { TextureRole::Normal, int(i) };
		}
	}
	const std::string name = to_lower_ascii(p_parameter);
	if (name.find("normal") != std::string::npos) {
		return { TextureRole::Normal, -1 };
	}
	if (name.find("diffuse") != std::string::npos || name.find("color") != std::string::npos || name.find("albedo") != std::string::npos) {
		return { TextureRole::Color, -1 };
	}
	return { TextureRole::Data, -1 };
}

std::pair<int32_t, int32_t> drawn_parameters(const MaterialSlot &p_slot) {
	int32_t albedo = -1, normal = -1;
	int albedo_rank = INT32_MAX, normal_rank = INT32_MAX;
	for (size_t i = 0; i < p_slot.textures.size(); ++i) {
		const ParameterUse use = parameter_use(p_slot.textures[i].first);
		if (!use.drawn() || p_slot.textures[i].second.empty()) {
			continue;
		}
		if (use.role == TextureRole::Color && use.rank < albedo_rank) {
			albedo = int32_t(i);
			albedo_rank = use.rank;
		} else if (use.role == TextureRole::Normal && use.rank < normal_rank) {
			normal = int32_t(i);
			normal_rank = use.rank;
		}
	}
	return { albedo, normal };
}

TextureRole merge_role(TextureRole p_a, TextureRole p_b) {
	const auto rank = [](TextureRole p_r) { return p_r == TextureRole::Normal ? 2 : p_r == TextureRole::Color ? 1 : 0; };
	return rank(p_a) >= rank(p_b) ? p_a : p_b;
}

ShaderFlags shader_flags(std::string_view p_shader) {
	ShaderFlags out;
	const std::string name = to_lower_ascii(p_shader);
	for (std::string_view w : kUndrawnShaders) {
		if (name.find(w) != std::string::npos) {
			out.drawn = false;
		}
	}
	out.alpha_test = contains_any(name, kAlphaTestWords);
	out.double_sided = contains_any(name, kDoubleSidedWords);
	return out;
}

bool has_triangles(const PreparedMesh &p_mesh) {
	for (const PreparedSurface &s : p_mesh.surfaces) {
		if (!s.indices.empty()) {
			return true;
		}
	}
	return false;
}

uint64_t prepared_bytes(const PreparedMesh &p_mesh) {
	uint64_t bytes = 0;
	for (const PreparedSurface &s : p_mesh.surfaces) {
		bytes += (s.positions.size() + s.normals.size() + s.tangents.size() + s.uv0.size() + s.uv1.size() + s.weights.size()) * sizeof(float);
		bytes += s.colors.size() + s.parts.size() * sizeof(uint16_t) + (s.bones.size() + s.indices.size()) * sizeof(int32_t);
	}
	return bytes;
}

uint64_t prepared_bytes(const PreparedTexture &p_texture) {
	return p_texture.data.size();
}

core::Status apply_scene_variant(PreparedMesh &r_mesh, const std::vector<uint16_t> &p_hidden, const std::optional<Mat3> &p_bake,
		const std::vector<uint32_t> &p_surface_materials) {
	std::vector<PreparedSurface> kept;
	kept.reserve(r_mesh.surfaces.size());
	const bool mirror = p_bake && det3(*p_bake) < 0.0f;
	const std::optional<Mat3> normal_bake = p_bake ? std::optional<Mat3>(normal_matrix(*p_bake)) : std::nullopt;
	for (PreparedSurface &s : r_mesh.surfaces) {
		if (s.material_index >= p_surface_materials.size() || p_surface_materials[s.material_index] == kSceneUndrawnMaterial) {
			continue;
		}
		BF3_TRY(check_surface(s, r_mesh.name));
		const size_t n = s.vertex_count();
		if (!p_hidden.empty() && !s.parts.empty()) {
			const auto hidden = [&](int32_t p_index) { return std::binary_search(p_hidden.begin(), p_hidden.end(), s.parts[size_t(p_index)]); };
			std::vector<int32_t> indices;
			indices.reserve(s.indices.size());
			for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
				if (hidden(s.indices[t]) || hidden(s.indices[t + 1]) || hidden(s.indices[t + 2])) {
					continue;
				}
				indices.insert(indices.end(), { s.indices[t], s.indices[t + 1], s.indices[t + 2] });
			}
			if (indices.size() != s.indices.size()) {
				s.indices = std::move(indices);
				std::vector<uint8_t> used(n, 0);
				for (int32_t i : s.indices) {
					used[size_t(i)] = 1;
				}
				compact_vertices(s, used);
			}
		}
		if (s.indices.empty()) {
			continue;
		}
		if (p_bake) {
			const size_t m = s.vertex_count();
			for (size_t i = 0; i < m; ++i) {
				float *p = &s.positions[i * 3];
				const Vec3 q = mul(*p_bake, { p[0], p[1], p[2] });
				p[0] = q[0];
				p[1] = q[1];
				p[2] = q[2];
				if (!s.normals.empty()) {
					float *nn = &s.normals[i * 3];
					const Vec3 r = normalized(mul(*normal_bake, { nn[0], nn[1], nn[2] }));
					nn[0] = r[0];
					nn[1] = r[1];
					nn[2] = r[2];
				}
				if (!s.tangents.empty()) {
					float *t = &s.tangents[i * 4];
					const Vec3 r = normalized(mul(*p_bake, { t[0], t[1], t[2] }));
					t[0] = r[0];
					t[1] = r[1];
					t[2] = r[2];
					if (mirror) {
						t[3] = -t[3];
					}
				}
			}
			if (mirror) {
				for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
					std::swap(s.indices[t + 1], s.indices[t + 2]);
				}
			}
		}
		recompute_aabb(s);
		kept.push_back(std::move(s));
	}
	r_mesh.surfaces = std::move(kept);
	r_mesh.vertices = 0;
	r_mesh.triangles = 0;
	for (const PreparedSurface &s : r_mesh.surfaces) {
		r_mesh.vertices += s.vertex_count();
		r_mesh.triangles += s.indices.size() / 3;
	}
	return core::Status::success();
}

uint8_t terrain_depth(const core::res::HeightfieldTree &p_tree, float p_spacing) {
	if (p_tree.nodes.empty() || p_tree.samples_per_side < 2 * p_tree.border_width + 2) {
		return 0;
	}
	const core::res::HeightfieldNode &root = p_tree.nodes.front();
	const size_t cells = p_tree.interior_cells();
	const float span = std::fmax(root.bounds_max[0] - root.bounds_min[0], root.bounds_max[2] - root.bounds_min[2]);
	const uint8_t max_level = p_tree.max_level();
	uint8_t depth = 0;
	while (depth < max_level && depth < 16 && span / float(cells << depth) > p_spacing * 1.001f && (cells << (depth + 1)) < core::res::kMaxGridSide) {
		++depth;
	}
	return depth;
}

core::Result<PreparedMesh> terrain_mesh(const core::res::HeightGrid &p_grid, const LevelTransform &p_transform, std::string_view p_name,
		const std::optional<std::array<float, 4>> &p_crop_xz) {
	const size_t w = p_grid.width;
	const size_t h = p_grid.height;
	const std::string where = "terrain " + std::string(p_name);
	if (w < 2 || h < 2 || w > core::res::kMaxGridSide || h > core::res::kMaxGridSide) {
		return core::make_error(where + ": grid " + std::to_string(w) + " x " + std::to_string(h) + " outside 2.." + std::to_string(core::res::kMaxGridSide));
	}
	if (p_grid.heights.size() != w * h || p_grid.holes.size() != w * h) {
		return core::make_error(where + ": grid arrays do not match its size");
	}
	const float ox = p_grid.origin_xz[0];
	const float oz = p_grid.origin_xz[1];
	const float sx = p_grid.sample_spacing[0];
	const float sz = p_grid.sample_spacing[1];
	if (!(sx > 0.0f) || !(sz > 0.0f) || !std::isfinite(ox) || !std::isfinite(oz) || !std::isfinite(sx) || !std::isfinite(sz)) {
		return core::make_error(where + ": bad grid origin or spacing");
	}
	size_t x0 = 0, x1 = w - 1, z0 = 0, z1 = h - 1;
	if (p_crop_xz) {
		const auto index = [](float p_v, float p_o, float p_s, size_t p_n, bool p_up) {
			const double f = (double(p_v) - double(p_o)) / double(p_s);
			const double r = p_up ? std::ceil(f) : std::floor(f);
			if (!(r > 0.0)) {
				return size_t(0);
			}
			return r >= double(p_n - 1) ? p_n - 1 : size_t(r);
		};
		x0 = index((*p_crop_xz)[0], ox, sx, w, false);
		z0 = index((*p_crop_xz)[1], oz, sz, h, false);
		x1 = index((*p_crop_xz)[2], ox, sx, w, true);
		z1 = index((*p_crop_xz)[3], oz, sz, h, true);
	}
	PreparedMesh out;
	out.name = std::string(p_name);
	if (x1 <= x0 || z1 <= z0) {
		return out;
	}
	const size_t cw = x1 - x0 + 1;
	const size_t ch = z1 - z0 + 1;
	std::vector<int32_t> vertex(cw * ch, -1);
	PreparedSurface s;
	s.material_name = "terrain";
	const bool mirror = p_transform.determinant() < 0.0f;
	const auto sample = [&](size_t p_x, size_t p_z) -> int32_t {
		int32_t &v = vertex[(p_x - x0) + (p_z - z0) * cw];
		if (v >= 0) {
			return v;
		}
		v = int32_t(s.positions.size() / 3);
		const Vec3 p = p_transform.transform_point({ ox + float(p_x) * sx, p_grid.at(p_x, p_z), oz + float(p_z) * sz });
		s.positions.insert(s.positions.end(), { p[0], p[1], p[2] });
		const size_t xa = p_x > 0 ? p_x - 1 : p_x;
		const size_t xb = p_x + 1 < w ? p_x + 1 : p_x;
		const size_t za = p_z > 0 ? p_z - 1 : p_z;
		const size_t zb = p_z + 1 < h ? p_z + 1 : p_z;
		const float dhdx = (p_grid.at(xb, p_z) - p_grid.at(xa, p_z)) / (float(xb - xa) * sx);
		const float dhdz = (p_grid.at(p_x, zb) - p_grid.at(p_x, za)) / (float(zb - za) * sz);
		const Vec3 n = normalized(p_transform.rotate(normalized({ -dhdx, 1.0f, -dhdz })));
		s.normals.insert(s.normals.end(), { n[0], n[1], n[2] });
		return v;
	};
	for (size_t z = z0; z < z1; ++z) {
		for (size_t x = x0; x < x1; ++x) {
			if (p_grid.hole(x, z) || p_grid.hole(x + 1, z) || p_grid.hole(x, z + 1) || p_grid.hole(x + 1, z + 1)) {
				continue;
			}
			const int32_t a = sample(x, z);
			const int32_t b = sample(x + 1, z);
			const int32_t c = sample(x, z + 1);
			const int32_t d = sample(x + 1, z + 1);

			if (mirror) {
				s.indices.insert(s.indices.end(), { a, c, b, b, c, d });
			} else {
				s.indices.insert(s.indices.end(), { a, b, c, b, d, c });
			}
		}
	}
	if (s.indices.empty()) {
		return out;
	}
	recompute_aabb(s);
	out.vertices = s.vertex_count();
	out.triangles = s.indices.size() / 3;
	out.surfaces.push_back(std::move(s));
	return out;
}

}

std::vector<SceneMeshRequest> scene_mesh_requests(const MeshCensus &p_census) {
	std::vector<SceneMeshRequest> out;
	struct Shape {
		size_t lods = 0;
		bool model = false;
	};
	std::vector<Shape> shapes;
	std::unordered_map<std::string, size_t> by_name;
	for (uint32_t v = 0; v < p_census.meshes.size(); ++v) {
		const SceneMesh &m = p_census.meshes[v];
		if (!m.kept) {
			continue;
		}
		auto [it, fresh] = by_name.emplace(m.name, out.size());
		if (fresh) {
			out.push_back(SceneMeshRequest{ m.name, MeshRequest{}, {} });
			shapes.push_back(Shape{});
		}
		out[it->second].variants.push_back(v);
		Shape &shape = shapes[it->second];
		shape.lods = std::max(shape.lods, m.lods.size());
		shape.model = shape.model || !m.world_kind;
	}
	for (size_t i = 0; i < out.size(); ++i) {
		MeshRequest &request = out[i].request;
		request.lods.clear();
		const size_t lods = shapes[i].model ? std::max<size_t>(shapes[i].lods, 1) : 1;
		for (size_t l = 0; l < lods; ++l) {
			request.lods.push_back(uint32_t(l));
		}
		request.filter = core::res::SubsetFilter::Drawn;
		request.lane_binds = nullptr;
	}
	return out;
}

core::Result<LevelScenePlan> plan_level_scene(const EbxResolver &p_resolver, const World &p_world, const MeshCensus &p_census, const ScenePlanOptions &p_options) {
	const Clock::time_point start = Clock::now();
	LevelScenePlan plan;
	plan.look = p_options.look;
	PlanBuilder builder{ plan, {}, {}, nullptr };
	std::unique_ptr<LookResolver> resolver;
	if (p_options.look == SceneLook::L1) {
		if (p_options.mount != nullptr) {
			resolver = LookResolver::open(*p_options.mount);
			for (const auto &[item, error] : resolver->failures()) {
				plan.failures.emplace_back(item, error);
			}
			builder.resolver = resolver.get();
		} else {
			plan.failures.emplace_back("look", "L1 materials need the level mount; M1 materials used");
		}
	}
	plan.requests = scene_mesh_requests(p_census);
	std::vector<uint32_t> request_of(p_census.meshes.size(), UINT32_MAX);
	for (uint32_t r = 0; r < plan.requests.size(); ++r) {
		for (uint32_t v : plan.requests[r].variants) {
			request_of[v] = r;
		}
	}

	std::vector<uint32_t> shown(p_census.meshes.size(), 0);
	std::vector<uint8_t> instance_shown(p_census.instances.size(), 1);
	for (size_t i = 0; i < p_census.instances.size(); ++i) {
		const core::level::SceneInstance &inst = p_census.instances[i];
		if (inst.mesh >= p_census.meshes.size() || inst.object >= p_world.objects.size() || request_of[inst.mesh] == UINT32_MAX) {
			return core::make_error("scene plan: census instance " + std::to_string(i) + " names no kept mesh or object");
		}
		if (!p_options.include_hidden_start) {
			core::Result<bool> hidden = core::level::starts_hidden(p_world, p_world.objects[inst.object]);
			if (!hidden) {
				return core::error_at("scene plan", hidden.error());
			}
			if (*hidden) {
				instance_shown[i] = 0;
				continue;
			}
		}
		++shown[inst.mesh];
	}

	std::vector<uint32_t> plan_mesh_of(p_census.meshes.size(), UINT32_MAX);
	std::unordered_map<std::string, uint32_t> neutral_material;
	for (uint32_t v = 0; v < p_census.meshes.size(); ++v) {
		const SceneMesh &m = p_census.meshes[v];
		if (!m.kept || shown[v] == 0) {
			continue;
		}
		SceneMeshPlan mesh;
		mesh.request = request_of[v];
		mesh.census_mesh = v;
		mesh.hidden_parts = m.hidden;
		std::sort(mesh.hidden_parts.begin(), mesh.hidden_parts.end());
		mesh.bake = m.bake;
		mesh.world_kind = m.world_kind;
		mesh.instances = shown[v];
		core::Result<MeshMaterials> materials = mesh_materials(p_resolver, m.asset, m.entry);
		if (materials) {
			mesh.surface_materials.reserve(materials->slots.size());
			for (const MaterialSlot &slot : materials->slots) {
				if (builder.resolver != nullptr) {
					mesh.surface_materials.push_back(builder.look_material(slot, m.name));
				} else {
					mesh.surface_materials.push_back(scene_internal::shader_flags(slot.shader).drawn ? builder.material(slot) : kSceneUndrawnMaterial);
				}
			}
		} else {

			plan.failures.emplace_back(m.name, materials.error().message);
			const size_t slots = std::min(p_resolver.follow_all(m.asset.data(), "Materials").size(), kMaxMaterialSlots);
			const uint32_t neutral = builder.material(MaterialSlot{});
			mesh.surface_materials.assign(std::max<size_t>(slots, 1), neutral);
		}
		plan_mesh_of[v] = uint32_t(plan.meshes.size());
		plan.meshes.push_back(std::move(mesh));
	}
	plan.instances.reserve(p_census.instances.size());
	for (size_t i = 0; i < p_census.instances.size(); ++i) {
		if (instance_shown[i] == 0) {
			continue;
		}
		const core::level::SceneInstance &inst = p_census.instances[i];
		plan.instances.push_back(SceneInstancePlan{ plan_mesh_of[inst.mesh], inst.transform });
	}

	if (p_options.include_terrain) {
		for (uint32_t at = 0; at < p_world.objects.size(); ++at) {
			const PlacedObject &object = p_world.objects[at];
			if (object.kind != ObjectKind::Terrain) {
				continue;
			}
			const std::optional<EbxInstance> asset = p_resolver.follow(object.entity.data(), "TerrainAsset");
			const std::optional<EbxValue> name = asset ? asset->get("Name") : std::nullopt;
			const std::optional<std::string_view> text = name ? name->as_str() : std::nullopt;
			if (!text || text->empty() || text->size() > 1024) {
				plan.failures.emplace_back("terrain object " + std::to_string(at), "no TerrainAsset name");
				continue;
			}
			SceneTerrainPlan terrain;
			terrain.object = at;
			terrain.asset = std::string(*text);
			terrain.transform = object.transform;
			plan.terrains.push_back(std::move(terrain));
		}
	}
	plan.environment = read_environment(p_resolver, p_world);
	if (resolver != nullptr) {
		plan.look_stats = resolver->stats();
		core::Result<LookEnvironment> env = read_look_environment(p_resolver, p_world);
		if (env) {
			plan.look_environment = std::move(*env);
			decode_look_textures(*p_options.mount, plan.look_environment);
			const LookEnvState &base = plan.look_environment.base;
			const auto sky_texture = [&](const std::string &p_name) { return p_name.empty() ? -1 : int32_t(builder.texture(p_name, TextureRole::Color)); };
			if (base.sky) {
				plan.sky_panorama = sky_texture(base.panorama);
				plan.sky_panorama_alpha = sky_texture(base.panorama_alpha);
				plan.sky_gradient = sky_texture(base.gradient);
			}
		} else {
			plan.failures.emplace_back("look environment", env.error().message);
		}
	}
	plan.seconds = seconds_since(start);
	return plan;
}

core::Result<LevelScenePlan> plan_level_scene(const core::level::LoadedLevel &p_level, const World &p_world, const MeshCensus &p_census,
		const ScenePlanOptions &p_options) {
	ScenePlanOptions options = p_options;
	if (options.mount == nullptr) {
		options.mount = &p_level.mount();
	}
	return plan_level_scene(p_level.db(), p_world, p_census, options);
}

core::Result<CachedMesh> load_scene_request(core::Cache *p_cache, const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_request) {
	if (p_request >= p_plan.requests.size()) {
		return core::make_error("scene request " + std::to_string(p_request) + " out of range");
	}
	const SceneMeshRequest &request = p_plan.requests[p_request];
	core::Result<MeshHead> head = read_mesh_head(p_mount, request.res);
	if (!head) {
		return head.error();
	}
	return load_mesh_cached(p_cache, p_mount, *head, request.request);
}

core::Result<PreparedSceneMesh> prepare_scene_mesh(core::Cache *p_cache, const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_mesh,
		const PreparedMeshSet &p_blob) {
	(void)p_cache;
	(void)p_mount;
	if (p_mesh >= p_plan.meshes.size()) {
		return core::make_error("scene mesh " + std::to_string(p_mesh) + " out of range");
	}
	const SceneMeshPlan &plan = p_plan.meshes[p_mesh];
	if (p_blob.meshes.size() != p_blob.lods.size()) {
		return core::make_error("scene mesh " + p_blob.name + ": blob LOD list and meshes differ");
	}

	std::vector<size_t> order(p_blob.meshes.size());
	for (size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p_blob.lods[a] < p_blob.lods[b]; });
	for (size_t i : order) {
		PreparedSceneMesh out;
		out.mesh = p_mesh;
		out.lod0 = p_blob.meshes[i];
		BF3_TRY(scene_internal::apply_scene_variant(out.lod0, plan.hidden_parts, plan.bake, plan.surface_materials));
		if (scene_internal::has_triangles(out.lod0)) {
			return out;
		}
	}
	return core::make_error("scene mesh " + p_blob.name + ": no LOD draws a triangle");
}

core::Result<PreparedTexture> prepare_scene_texture(const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_texture) {
	if (p_texture >= p_plan.textures.size()) {
		return core::make_error("scene texture " + std::to_string(p_texture) + " out of range");
	}
	const SceneTexturePlan &plan = p_plan.textures[p_texture];
	core::Result<core::res::LoadedTexture> texture = core::res::load_texture(p_mount, plan.name);
	if (!texture) {
		return texture.error();
	}
	return prepare_texture(*texture, plan.role);
}

core::Result<PreparedMesh> prepare_scene_terrain(const core::LevelMount &p_mount, const LevelScenePlan &p_plan, uint32_t p_terrain,
		const SceneTerrainOptions &p_options) {
	if (p_terrain >= p_plan.terrains.size()) {
		return core::make_error("scene terrain " + std::to_string(p_terrain) + " out of range");
	}
	const SceneTerrainPlan &plan = p_plan.terrains[p_terrain];
	const std::string where = "terrain " + plan.asset;
	if (!(p_options.spacing > 0.0f) || !std::isfinite(p_options.spacing)) {
		return core::make_error(where + ": bad spacing");
	}
	auto terrain = p_mount.read_res(plan.asset);
	if (!terrain) {
		return core::error_at("res " + plan.asset, terrain.error());
	}
	std::string tree_name;
	core::res::ResError e = core::res::terrain_streaming_tree_name(terrain->second, tree_name);
	if (!e.ok()) {
		return core::make_error("res " + plan.asset + ": " + e.message());
	}
	auto tree_bytes = p_mount.read_res(tree_name);
	if (!tree_bytes) {
		return core::error_at("res " + tree_name, tree_bytes.error());
	}
	core::res::TerrainStreamingTree tree;
	e = core::res::TerrainStreamingTree::parse(tree_bytes->second, tree);
	if (!e.ok()) {
		return core::make_error("res " + tree_name + ": " + e.message());
	}
	if (!tree.heightfield || tree.heightfield->nodes.empty()) {
		return core::make_error("res " + tree_name + ": streaming tree without a heightfield");
	}
	const uint8_t depth = scene_internal::terrain_depth(*tree.heightfield, p_options.spacing);
	std::map<ChunkKey, std::optional<std::vector<uint8_t>>> chunks;
	const auto fetch = [&](const core::res::ChunkId &p_id) -> std::optional<std::vector<uint8_t>> {
		const ChunkKey key{ p_id.bytes };
		const auto found = chunks.find(key);
		if (found != chunks.end()) {
			return found->second;
		}
		if (chunks.size() >= kMaxTerrainChunks) {
			return std::nullopt;
		}
		core::Result<std::vector<uint8_t>> chunk = p_mount.read_chunk(core::Guid::from_bytes(p_id.bytes.data()));
		std::optional<std::vector<uint8_t>> value;
		if (chunk) {
			value = std::move(*chunk);
		}
		return chunks.emplace(key, std::move(value)).first->second;
	};
	core::res::HeightGrid grid;
	e = tree.height_grid(depth, fetch, grid);
	if (!e.ok()) {
		return core::make_error("res " + tree_name + ": " + e.message());
	}
	chunks.clear();

	std::optional<std::array<float, 4>> crop;
	const LevelTransform &t = plan.transform;
	const bool axis_aligned = t.right == Vec3{ 1.0f, 0.0f, 0.0f } && t.up == Vec3{ 0.0f, 1.0f, 0.0f } && t.forward == Vec3{ 0.0f, 0.0f, 1.0f } && t.scale > 0.0f;
	if (axis_aligned && !p_plan.instances.empty()) {
		std::array<float, 4> box = { INFINITY, INFINITY, -INFINITY, -INFINITY };
		for (const SceneInstancePlan &inst : p_plan.instances) {
			const float x = (inst.transform.trans[0] - t.trans[0]) / t.scale;
			const float z = (inst.transform.trans[2] - t.trans[2]) / t.scale;
			if (!std::isfinite(x) || !std::isfinite(z)) {
				continue;
			}
			box[0] = std::fmin(box[0], x);
			box[1] = std::fmin(box[1], z);
			box[2] = std::fmax(box[2], x);
			box[3] = std::fmax(box[3], z);
		}
		if (box[0] <= box[2]) {
			const float margin = kTerrainMargin / t.scale;
			crop = std::array<float, 4>{ box[0] - margin, box[1] - margin, box[2] + margin, box[3] + margin };
		}
	}
	return scene_internal::terrain_mesh(grid, t, plan.asset, crop);
}

uint64_t estimate_scene_bytes(const LevelScenePlan &p_plan, const core::LevelMount &p_mount) {
	uint64_t bytes = 0;
	std::vector<uint32_t> variants(p_plan.requests.size(), 0);
	for (const SceneMeshPlan &m : p_plan.meshes) {
		if (m.request < variants.size()) {
			++variants[m.request];
		}
	}

	constexpr uint64_t kVertexBytes = 60;
	for (size_t r = 0; r < p_plan.requests.size(); ++r) {
		if (variants[r] == 0) {
			continue;
		}
		core::Result<MeshHead> head = read_mesh_head(p_mount, p_plan.requests[r].res);
		if (!head || head->mesh.lods.empty()) {
			continue;
		}
		uint64_t lod = 0;
		for (const core::res::MeshSubset &s : head->mesh.lods[0].subsets) {
			lod += uint64_t(s.vertex_count) * kVertexBytes + uint64_t(s.primitive_count) * 3 * sizeof(int32_t);
		}
		bytes += lod * variants[r];
	}
	for (const SceneTexturePlan &t : p_plan.textures) {
		core::Result<core::res::RawRes> raw = core::res::read_raw_res(p_mount, t.name);
		if (!raw) {
			continue;
		}
		core::res::DxTexture texture;
		if (!core::res::DxTexture::parse(raw->bytes(), texture).ok()) {
			continue;
		}
		const int64_t level0 = core::res::texture_image_bytes(texture.format, texture.width, texture.height);
		if (level0 > 0) {
			bytes += uint64_t(level0) + uint64_t(level0) / 3;
		}
	}
	return bytes;
}

}
