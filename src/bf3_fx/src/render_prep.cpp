#include "bf3/fx/render_prep.h"

#include "bf3/core/math.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf3::fx {

namespace {

inline Vec3 sub(const Vec3 &a, const Vec3 &b) {
	return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z };
}
inline float dot(const Vec3 &a, const Vec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 scale(const Vec3 &a, float s) {
	return Vec3{ a.x * s, a.y * s, a.z * s };
}
inline Vec3 add(const Vec3 &a, const Vec3 &b) {
	return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z };
}
inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
	return Vec3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float length(const Vec3 &a) {
	return std::sqrt(dot(a, a));
}
inline Vec3 normalized(const Vec3 &a, const Vec3 &p_fallback) {
	const float l2 = dot(a, a);
	if (!(l2 > 1e-20f) || !std::isfinite(l2)) {
		return p_fallback;
	}
	return scale(a, 1.0f / std::sqrt(l2));
}

inline void put_transform(float *r_t, const Vec3 &x, const Vec3 &y, const Vec3 &z, const Vec3 &o) {
	r_t[0] = x.x;
	r_t[1] = y.x;
	r_t[2] = z.x;
	r_t[3] = o.x;
	r_t[4] = x.y;
	r_t[5] = y.y;
	r_t[6] = z.y;
	r_t[7] = o.y;
	r_t[8] = x.z;
	r_t[9] = y.z;
	r_t[10] = z.z;
	r_t[11] = o.z;
}

struct Resolved {
	std::string texture;
	std::string normal_map;
	const AtlasEntry *base = nullptr;
	const AtlasEntry *normal = nullptr;
	bool fallback = false;
	bool untextured = false;
};

constexpr size_t kMaxResolved = 1u << 16;

struct OutputRef {
	uint32_t output = 0;
	float depth = 0;
};

}

FxBatchKey fx_batch_key(const EmitterDef &p_emitter, bool p_first_person) {
	FxBatchKey k;
	switch (p_emitter.tmpl.emittable) {
		case EmittableType::DirectionAlignedQuad:
			k.shader = FxShaderClass::Daq;
			break;
		case EmittableType::WorldAlignedQuad:
		case EmittableType::Quad:
			k.shader = FxShaderClass::Waq;
			break;
		case EmittableType::ParticleMesh:
			k.shader = FxShaderClass::Mesh;
			break;
		case EmittableType::ScreenAlignedQuad:
		case EmittableType::Trail:
		case EmittableType::Point:
		default:
			k.shader = FxShaderClass::Saq;
			break;
	}
	k.blend = (p_emitter.tmpl.flags & kEmitterEmissive) != 0 ? FxBlend::Additive : FxBlend::Alpha;
	k.pass = p_first_person ? FxPass::FirstPerson : FxPass::World;
	return k;
}

float fx_distance_scale(const EmitterTemplateDef &p_template, float p_distance) {
	const float near_scale = p_template.distance_scale_near, far_scale = p_template.distance_scale_far;
	if (!std::isfinite(near_scale) || !std::isfinite(far_scale) || !std::isfinite(p_distance)) {
		return 1.0f;
	}
	const float t = p_template.distance_scale_length > 0.0f
			? std::clamp(p_distance / p_template.distance_scale_length, 0.0f, 1.0f)
			: 1.0f;
	return near_scale + (far_scale - near_scale) * t;
}

FxViewAxes fx_view_axes(const Vec3 &p_forward) {
	FxViewAxes axes;
	const Vec3 forward = normalized(p_forward, Vec3{ 0, 0, 1 });
	axes.right = normalized(cross(forward, Vec3{ 0, 1, 0 }), Vec3{ 1, 0, 0 });
	axes.up = normalized(cross(axes.right, forward), Vec3{ 0, 1, 0 });
	return axes;
}

Vec3 fx_pivoted_position(FxShaderClass p_shader, bool p_world_aligned, const Vec2 &p_pivot,
		const ParticleVertex &p_particle, const Vec3 &p_emitter_right, const Vec3 &p_emitter_up,
		const Vec3 &p_view_right, const Vec3 &p_view_up) {
	if ((p_pivot.x == 0.0f && p_pivot.y == 0.0f) || !std::isfinite(p_pivot.x) || !std::isfinite(p_pivot.y)) {
		return p_particle.position;
	}
	const float along_x = p_pivot.x * p_particle.size.x * 0.5f, along_y = p_pivot.y * p_particle.size.y * 0.5f;
	const float c = std::cos(p_particle.rotation), s = std::sin(p_particle.rotation);
	switch (p_shader) {
		case FxShaderClass::Saq: {
			const float right = along_x * c - along_y * s, up = along_x * s + along_y * c;
			return sub(p_particle.position, add(scale(p_view_right, right), scale(p_view_up, up)));
		}
		case FxShaderClass::Daq: {
			const Vec3 axis = normalized(p_particle.axis, Vec3{ 0, 1, 0 });
			const float dx = dot(axis, p_view_right), dy = dot(axis, p_view_up);
			const float len = std::sqrt(dx * dx + dy * dy);
			const float ux = len > 1e-5f ? dx / len : 0.0f, uy = len > 1e-5f ? dy / len : 1.0f;
			const float right = uy * along_x + ux * along_y, up = -ux * along_x + uy * along_y;
			return sub(p_particle.position, add(scale(p_view_right, right), scale(p_view_up, up)));
		}
		case FxShaderClass::Waq: {
			const Vec3 bx = p_world_aligned ? Vec3{ 1, 0, 0 } : p_emitter_right;
			const Vec3 by = p_world_aligned ? Vec3{ 0, 0, -1 } : p_emitter_up;
			const Vec3 rx = add(scale(bx, c), scale(by, s));
			const Vec3 ry = add(scale(bx, -s), scale(by, c));
			return sub(p_particle.position, add(scale(rx, along_x), scale(ry, along_y)));
		}
		default:
			return p_particle.position;
	}
}

float fx_pack_alpha_levels(float p_min, float p_max) {
	auto q = [](float v) {
		if (!std::isfinite(v)) {
			return 0.0f;
		}
		return std::floor(std::clamp(v, 0.0f, 1.0f) * float(kFxAlphaLevelSteps) + 0.5f);
	};
	const float mn = q(p_min);
	const float mx = std::isfinite(p_max) ? q(p_max) : float(kFxAlphaLevelSteps);
	return mn * float(kFxAlphaLevelSteps + 1) + mx;
}

void fx_unpack_alpha_levels(float p_packed, float &r_min, float &r_max) {
	const float s = float(kFxAlphaLevelSteps + 1);
	const float mn = std::floor(p_packed / s);
	r_min = mn / float(kFxAlphaLevelSteps);
	r_max = (p_packed - mn * s) / float(kFxAlphaLevelSteps);
}

size_t fx_batch_index(const FxBatchKey &p_key) {
	return (size_t(p_key.pass) * kFxBlendCount + size_t(p_key.blend)) * kFxShaderClassCount + size_t(p_key.shader);
}

FxBatchKey fx_batch_key_at(size_t p_index) {
	FxBatchKey k;
	const size_t i = p_index % kMaxFxBatches;
	k.shader = FxShaderClass(i % kFxShaderClassCount);
	k.blend = FxBlend((i / kFxShaderClassCount) % kFxBlendCount);
	k.pass = FxPass(i / (kFxShaderClassCount * kFxBlendCount));
	return k;
}

namespace {
constexpr std::array<FxBlend, kFxBlendCount> kBlendDrawOrder{ FxBlend::Alpha, FxBlend::Premultiplied, FxBlend::Additive };
}

struct RenderPrep::Impl {
	std::vector<FxInstance> instances;
	std::vector<FxBatch> batches;
	std::vector<FxLight> lights;
	std::vector<FxLight> candidates;
	std::vector<FxEmitterParams> params;
	std::vector<const std::string *> row_meshes;
	std::array<std::vector<OutputRef>, kMaxFxBatches> per_batch;
	std::unordered_map<const EmitterDef *, Resolved> resolved;
	const EmitterAtlas *atlas = nullptr;
	const AtlasEntry *default_entry = nullptr;
	RenderPrepStats stats;

	const Resolved &resolve(const EmitterDef &p_e, const EmitterAtlas &p_atlas) {
		if (resolved.size() >= kMaxResolved) {
			resolved.clear();
		}
		Resolved &r = resolved[&p_e];
		if (r.base != nullptr || r.fallback || r.untextured) {
			if (r.texture == p_e.texture.texture && r.normal_map == p_e.texture.normal_map) {
				return r;
			}
		}
		r.texture = p_e.texture.texture;
		r.normal_map = p_e.texture.normal_map;
		r.untextured = r.texture.empty() && p_e.tmpl.emittable == EmittableType::ParticleMesh;
		r.base = r.texture.empty() ? nullptr : p_atlas.resolve(r.texture);
		if (r.base != nullptr && r.base->kind != AtlasKind::Base) {
			r.base = nullptr;
		}

		r.fallback = r.base == nullptr && !r.untextured;
		if (r.fallback) {
			r.base = default_entry;
		}
		r.normal = r.normal_map.empty() ? nullptr : p_atlas.resolve(r.normal_map);
		if (r.normal != nullptr && (r.normal->kind != AtlasKind::Normal || r.normal->degenerate)) {
			r.normal = nullptr;
		}
		return r;
	}
};

RenderPrep::RenderPrep() :
		impl_(std::make_unique<Impl>()) {
	impl_->batches.reserve(kMaxFxBatches);
	impl_->lights.reserve(kMaxFxLights);
}

RenderPrep::~RenderPrep() = default;

void RenderPrep::prepare(const FxWorld &p_world, const FxView &p_view, const EmitterAtlas &p_atlas) {
	const auto t0 = std::chrono::steady_clock::now();
	Impl &m = *impl_;
	if (m.atlas != &p_atlas) {
		m.atlas = &p_atlas;
		m.resolved.clear();
		m.default_entry = p_atlas.resolve(kFxDefaultTexture);
		if (m.default_entry != nullptr && m.default_entry->kind != AtlasKind::Base) {
			m.default_entry = nullptr;
		}
	}
	const std::span<const EmitterOutput> outputs = p_world.outputs();
	const std::span<const ParticleVertex> particles = p_world.particles();
	const std::span<const ParticleLight> plights = p_world.lights();
	const Vec3 cam = p_view.camera_position;
	const Vec3 fwd = normalized(p_view.camera_forward, Vec3{ 0, 0, 1 });
	const FxViewAxes axes = fx_view_axes(fwd);
	const Vec3 view_right = axes.right;
	const Vec3 view_up = axes.up;

	m.params.resize(outputs.size());
	m.row_meshes.assign(outputs.size(), nullptr);
	m.candidates.clear();
	for (std::vector<OutputRef> &b : m.per_batch) {
		b.clear();
	}
	uint32_t unresolved = 0;
	for (size_t i = 0; i < outputs.size(); ++i) {
		const EmitterOutput &o = outputs[i];
		FxEmitterParams &row = m.params[i];
		std::memset(&row, 0, sizeof(row));
		if (o.def == nullptr || o.emitter >= o.def->emitters.size()) {
			continue;
		}
		const EmitterDef &e = o.def->emitters[o.emitter];
		if (e.tmpl.emittable == EmittableType::ParticleMesh && !e.tmpl.mesh.empty()) {
			m.row_meshes[i] = &e.tmpl.mesh;
		}
		const Resolved &r = m.resolve(e, p_atlas);
		if (r.fallback && o.particle_count > 0) {
			++unresolved;
		}
		if (r.base != nullptr) {
			row.base_rect[0] = r.base->min_uv.x;
			row.base_rect[1] = r.base->min_uv.y;
			row.base_rect[2] = r.base->max_uv.x - r.base->min_uv.x;
			row.base_rect[3] = r.base->max_uv.y - r.base->min_uv.y;
		} else if (r.untextured) {
			row.base_rect[2] = kFxRectWhite;
			row.base_rect[3] = kFxRectWhite;
		}
		row.flipbook[0] = float(std::max<uint16_t>(1, e.texture.frame_count));
		row.flipbook[1] = float(std::clamp<uint16_t>(e.texture.frame_columns, 1, std::max<uint16_t>(1, e.texture.frame_count)));
		row.flipbook[2] = e.params.alpha_exponent;
		row.flipbook[3] = e.tmpl.soft_particles_fade;
		if (r.normal != nullptr) {
			row.normal_rect[0] = r.normal->min_uv.x;
			row.normal_rect[1] = r.normal->min_uv.y;
			row.normal_rect[2] = r.normal->max_uv.x - r.normal->min_uv.x;
			row.normal_rect[3] = r.normal->max_uv.y - r.normal->min_uv.y;
		}
		row.lighting[0] = e.tmpl.light_multiplier;
		row.lighting[1] = e.tmpl.light_wrap_around;
		row.lighting[2] = (e.tmpl.flags & kEmitterEmissive) != 0 ? 1.0f : 0.0f;
		row.lighting[3] = e.params.cull_threshold;

		const float depth = dot(sub(o.emitter_transform.trans, cam), fwd);
		if (o.particle_count > 0 && size_t(o.first_particle) + o.particle_count <= particles.size()) {
			m.per_batch[fx_batch_index(fx_batch_key(e, o.first_person))].push_back(OutputRef{ uint32_t(i), std::isfinite(depth) ? depth : 0.0f });
		}

		if (o.light_count > 0 && size_t(o.first_light) + o.light_count <= plights.size()) {
			FxLight l;
			double sum = 0, px = 0, py = 0, pz = 0, cr = 0, cg = 0, cb = 0;
			float radius = 0;
			for (uint32_t k = 0; k < o.light_count; ++k) {
				const ParticleLight &pl = plights[o.first_light + k];
				const double w = std::max(0.0f, pl.intensity);
				sum += w;
				px += w * pl.position.x;
				py += w * pl.position.y;
				pz += w * pl.position.z;
				cr += pl.color.x;
				cg += pl.color.y;
				cb += pl.color.z;
				radius = std::max(radius, pl.radius);
			}
			if (sum > 0.0 && radius > 0.0f) {
				l.position = Vec3{ float(px / sum), float(py / sum), float(pz / sum) };
				l.intensity = float(sum / double(o.light_count));

				l.color = Vec3{ float(cr / sum), float(cg / sum), float(cb / sum) };
				l.radius = radius;
				const Vec3 d = sub(l.position, cam);
				l.priority = l.intensity / std::max(dot(d, d), kFxLightMinDistance2);
				l.first_person = o.first_person;
				l.effect = o.effect;
				l.emitter = o.emitter;
				if (std::isfinite(l.priority) && std::isfinite(l.color.x + l.color.y + l.color.z)) {
					m.candidates.push_back(l);
				}
			}
		}
	}

	m.batches.clear();
	m.instances.resize(particles.size());
	uint32_t written = 0, mesh_instances = 0, first_person = 0;
	for (size_t pass = 0; pass < kFxPassCount; ++pass) {
		for (const FxBlend blend : kBlendDrawOrder) {
			for (size_t sc = 0; sc < kFxShaderClassCount; ++sc) {
				const FxBatchKey key{ FxShaderClass(sc), blend, FxPass(pass) };
				std::vector<OutputRef> &list = m.per_batch[fx_batch_index(key)];
				if (list.empty()) {
					continue;
				}
				if (blend != FxBlend::Additive) {
					std::stable_sort(list.begin(), list.end(), [](const OutputRef &a, const OutputRef &b) { return a.depth > b.depth; });
				}
				FxBatch batch;
				batch.key = key;
				batch.first_instance = written;
				Vec3 lo{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
				Vec3 hi{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
				float extent = 0;
				for (const OutputRef &ref : list) {
					const EmitterOutput &o = outputs[ref.output];
					const float row = float(ref.output);
					const Transform &et = o.emitter_transform;
					const bool world_aligned = o.def->emitters[o.emitter].tmpl.emittable == EmittableType::WorldAlignedQuad;
					const Vec3 ex = normalized(et.right, Vec3{ 1, 0, 0 });
					const Vec3 ey = normalized(et.up, Vec3{ 0, 1, 0 });
					const Vec3 ez = normalized(et.forward, Vec3{ 0, 0, 1 });
					const EmitterDef &drawn = o.def->emitters[o.emitter];
					++batch.emitters;
					for (uint32_t k = 0; k < o.particle_count; ++k) {
						ParticleVertex p = particles[o.first_particle + k];
						if (written >= m.instances.size()) {
							break;
						}
						if (!o.first_person) {
							const float s = fx_distance_scale(drawn.tmpl, length(sub(p.position, cam)));
							p.size = Vec3{ p.size.x * s, p.size.y * s, p.size.z * s };
						}
						p.position = fx_pivoted_position(
								key.shader, world_aligned, drawn.params.size_pivot, p, ex, ey, view_right, view_up);
						FxInstance &in = m.instances[written++];
						const float c = std::cos(p.rotation), s = std::sin(p.rotation);
						switch (key.shader) {
							case FxShaderClass::Saq:
								put_transform(in.transform, Vec3{ p.size.x * c, p.size.x * s, 0 }, Vec3{ -p.size.y * s, p.size.y * c, 0 }, Vec3{}, p.position);
								break;
							case FxShaderClass::Daq:
								put_transform(in.transform, Vec3{ p.size.x, 0, 0 }, Vec3{ 0, p.size.y, 0 }, normalized(p.axis, Vec3{ 0, 1, 0 }), p.position);
								break;
							case FxShaderClass::Waq: {

								const Vec3 bx = world_aligned ? Vec3{ 1, 0, 0 } : ex;
								const Vec3 by = world_aligned ? Vec3{ 0, 0, -1 } : ey;
								const Vec3 bn = world_aligned ? Vec3{ 0, 1, 0 } : ez;
								const Vec3 rx = add(scale(bx, c), scale(by, s));
								const Vec3 ry = add(scale(bx, -s), scale(by, c));
								put_transform(in.transform, scale(rx, p.size.x), scale(ry, p.size.y), bn, p.position);
								break;
							}
							case FxShaderClass::Mesh:
							default: {

								const float yx = p.axis.y * core::kDegToRadF, xx = p.axis.x * core::kDegToRadF, zx = p.axis.z * core::kDegToRadF;
								const float cy = std::cos(yx), sy = std::sin(yx), cx = std::cos(xx), sx = std::sin(xx), cz = std::cos(zx), sz = std::sin(zx);

								const Vec3 c0{ cy * cz + sy * sx * sz, cx * sz, -sy * cz + cy * sx * sz };
								const Vec3 c1{ -cy * sz + sy * sx * cz, cx * cz, sy * sz + cy * sx * cz };
								const Vec3 c2{ sy * cx, -sx, cy * cx };
								const float sz3 = p.size.z != 0.0f ? p.size.z : p.size.x;
								put_transform(in.transform, scale(c0, p.size.x), scale(c1, p.size.y), scale(c2, sz3),
										p.position);
								++mesh_instances;
								break;
							}
						}
						in.color[0] = p.color.x;
						in.color[1] = p.color.y;
						in.color[2] = p.color.z;
						in.color[3] = p.color.w;
						in.custom[0] = row;
						in.custom[1] = p.frame;
						in.custom[2] = fx_pack_alpha_levels(p.alpha_min, p.alpha_max);
						in.custom[3] = std::isfinite(p.alpha_scale) ? p.alpha_scale : 1.0f;
						lo = Vec3{ std::min(lo.x, p.position.x), std::min(lo.y, p.position.y), std::min(lo.z, p.position.z) };
						hi = Vec3{ std::max(hi.x, p.position.x), std::max(hi.y, p.position.y), std::max(hi.z, p.position.z) };
						extent = std::max(extent, std::max(std::fabs(p.size.x), std::max(std::fabs(p.size.y), std::fabs(p.size.z))));
					}
				}
				batch.instance_count = written - batch.first_instance;
				if (batch.instance_count == 0) {
					continue;
				}
				batch.aabb_min = sub(lo, Vec3{ extent, extent, extent });
				batch.aabb_max = add(hi, Vec3{ extent, extent, extent });
				if (key.pass == FxPass::FirstPerson) {
					first_person += batch.instance_count;
				}
				m.batches.push_back(batch);
			}
		}
	}
	m.instances.resize(written);

	const size_t keep = std::min(m.candidates.size(), kMaxFxLights);
	auto by_priority = [](const FxLight &a, const FxLight &b) { return a.priority > b.priority; };
	if (m.candidates.size() > keep) {
		std::nth_element(m.candidates.begin(), m.candidates.begin() + ptrdiff_t(keep), m.candidates.end(), by_priority);
	}
	m.lights.assign(m.candidates.begin(), m.candidates.begin() + ptrdiff_t(keep));
	std::sort(m.lights.begin(), m.lights.end(), by_priority);

	RenderPrepStats &st = m.stats;
	st.instances = written;
	st.batches = uint32_t(m.batches.size());
	st.lights = uint32_t(m.lights.size());
	st.light_candidates = uint32_t(m.candidates.size());
	st.lights_dropped += m.candidates.size() - keep;
	st.unresolved_textures += unresolved;
	st.unresolved_this_frame = unresolved;
	st.mesh_instances = mesh_instances;
	st.first_person_instances = first_person;
	++st.frames;
	st.prepare_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::span<const FxInstance> RenderPrep::instances() const {
	return impl_->instances;
}

std::span<const FxBatch> RenderPrep::batches() const {
	return impl_->batches;
}

std::span<const FxLight> RenderPrep::lights() const {
	return impl_->lights;
}

std::span<const FxEmitterParams> RenderPrep::emitter_params() const {
	return impl_->params;
}

const std::string *RenderPrep::row_mesh(uint32_t p_row) const {
	return p_row < impl_->row_meshes.size() ? impl_->row_meshes[p_row] : nullptr;
}

const RenderPrepStats &RenderPrep::stats() const {
	return impl_->stats;
}

}
