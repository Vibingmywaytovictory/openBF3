// VFX2 render preparation (docs/design/m2a.md §12.1; docs/design/m2a-fx2.md). See render_prep.h.
#include "bf3/fx/render_prep.h"

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

constexpr float kDegToRad = 0.017453292519943295f;

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
inline Vec3 normalized(const Vec3 &a, const Vec3 &p_fallback) {
	const float l2 = dot(a, a);
	if (!(l2 > 1e-20f) || !std::isfinite(l2)) {
		return p_fallback;
	}
	return scale(a, 1.0f / std::sqrt(l2));
}

// Writes the 3x4 row-major [X Y Z | O] of Godot's MultiMesh transform.
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
	std::string texture; // the emitter's names this entry was resolved for (content check, so a reused pointer is safe)
	std::string normal_map;
	const AtlasEntry *base = nullptr;
	const AtlasEntry *normal = nullptr;
	bool fallback = false; // base came from kFxDefaultTexture (or nothing resolved)
	bool untextured = false; // a ParticleMesh emitter without a texture name (drawn white, not a fallback)
};

constexpr size_t kMaxResolved = 1u << 16;

struct OutputRef {
	uint32_t output = 0;
	float depth = 0;
};

} // namespace

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

// Draw order inside a pass: alpha and premultiplied (sorted) first, additive last. fx_batch_index orders blends
// Alpha, Additive, Premultiplied, so the draw order is its own table.
namespace {
constexpr std::array<FxBlend, kFxBlendCount> kBlendDrawOrder{ FxBlend::Alpha, FxBlend::Premultiplied, FxBlend::Additive };
}

struct RenderPrep::Impl {
	std::vector<FxInstance> instances;
	std::vector<FxBatch> batches;
	std::vector<FxLight> lights;
	std::vector<FxLight> candidates;
	std::vector<FxEmitterParams> params;
	std::array<std::vector<OutputRef>, kMaxFxBatches> per_batch;
	std::unordered_map<const EmitterDef *, Resolved> resolved;
	const EmitterAtlas *atlas = nullptr;
	const AtlasEntry *default_entry = nullptr;
	RenderPrepStats stats;

	const Resolved &resolve(const EmitterDef &p_e, const EmitterAtlas &p_atlas) {
		if (resolved.size() >= kMaxResolved) {
			resolved.clear(); // bounded: defs come and go in a long session; a re-resolve is cheap
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
			r.base = nullptr; // a normal-atlas entry named as a diffuse texture: not drawable from the base atlas
		}
		// A mesh particle naming no texture draws with its mesh's own material in BF3 (the stand-in: white); every
		// other emitter without a resolved texture draws the default entry and is counted.
		r.fallback = r.base == nullptr && !r.untextured;
		if (r.fallback) {
			r.base = default_entry;
		}
		r.normal = r.normal_map.empty() ? nullptr : p_atlas.resolve(r.normal_map);
		if (r.normal != nullptr && (r.normal->kind != AtlasKind::Normal || r.normal->degenerate)) {
			r.normal = nullptr; // the defaultnormalmap aliases are degenerate: a flat normal, same as none
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

	m.params.resize(outputs.size());
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
			continue; // never from FxWorld; a hostile caller's record draws nothing
		}
		const EmitterDef &e = o.def->emitters[o.emitter];
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
		// Lights: one merged light per output (m2a.md §12.1).
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
				l.intensity = float(sum);
				// ParticleLight::color is PointLightColor x intensity: the intensity-weighted mean colour is sum / sum(I).
				l.color = Vec3{ float(cr / sum), float(cg / sum), float(cb / sum) };
				l.radius = radius;
				const Vec3 d = sub(l.position, cam);
				l.priority = l.intensity / std::max(dot(d, d), kFxLightMinDistance2);
				l.first_person = o.first_person;
				if (std::isfinite(l.priority) && std::isfinite(l.color.x + l.color.y + l.color.z)) {
					m.candidates.push_back(l);
				}
			}
		}
	}

	// Batches in draw order; transparent ones back-to-front by emitter depth (stable: equal depths keep list order).
	m.batches.clear();
	m.instances.resize(particles.size());
	uint32_t written = 0, standins = 0, first_person = 0;
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
					++batch.emitters;
					for (uint32_t k = 0; k < o.particle_count; ++k) {
						const ParticleVertex &p = particles[o.first_particle + k];
						if (written >= m.instances.size()) {
							break; // outputs overlapping in particles (never from FxWorld): stop at the particle count
						}
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
								// WorldAlignedQuad: the world XZ plane, normal +Y; Quad: the emitter's right / up plane, normal forward.
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
								// Euler degrees (SpawnOrientation), applied Y (yaw), X (pitch), Z (roll) [inferred].
								const float yx = p.axis.y * kDegToRad, xx = p.axis.x * kDegToRad, zx = p.axis.z * kDegToRad;
								const float cy = std::cos(yx), sy = std::sin(yx), cx = std::cos(xx), sx = std::sin(xx), cz = std::cos(zx), sz = std::sin(zx);
								// R = Ry * Rx * Rz, columns.
								const Vec3 c0{ cy * cz + sy * sx * sz, cx * sz, -sy * cz + cy * sx * sz };
								const Vec3 c1{ -cy * sz + sy * sx * cz, cx * cz, sy * sz + cy * sx * cz };
								const Vec3 c2{ sy * cx, -sx, cy * cx };
								const float sz3 = p.size.z != 0.0f ? p.size.z : p.size.x;
								const float u = kFxMeshStandinMeters;
								put_transform(in.transform, scale(c0, p.size.x * u), scale(c1, p.size.y * u), scale(c2, sz3 * u), p.position);
								++standins;
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

	// The light pool: highest priority first, at most kMaxFxLights; the rest dropped and counted.
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
	st.mesh_standins = standins;
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

const RenderPrepStats &RenderPrep::stats() const {
	return impl_->stats;
}

} // namespace bf3::fx
