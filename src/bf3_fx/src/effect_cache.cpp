#include "bf3/fx/effect_cache.h"

#include "bf3/core/cache/blob_io.h"
#include "bf3/core/cache/digest.h"
#include "bf3/fx/effect_compiler.h"
#include "fx_ebx.h"
#include "fx_internal.h"

#include <cmath>
#include <string>
#include <utility>

namespace bf3::fx {

namespace {

constexpr const char *kEffectMagic = "OBF3FXEF";
constexpr const char *kDepsMagic = "OBF3FXDP";
constexpr size_t kMaxDeps = 1u << 16;
constexpr uint32_t kKnownEmitterFlags = 0x7FFu | kEmitterDerived | kEmitterChild | kEmitterHasParameterisedEvaluator;

class Out {
public:
	static constexpr bool kReading = false;
	core::BlobWriter w;
	void f32(float &p_v) { w.f32(p_v); }
	void u8(uint8_t &p_v) { w.u8(p_v); }
	void u16(uint16_t &p_v) { w.u16(p_v); }
	void u32(uint32_t &p_v) { w.u32(p_v); }
	void i16(int16_t &p_v) { w.u16(uint16_t(p_v)); }
	void i32(int32_t &p_v) { w.i32(p_v); }
	void boolean(bool &p_v) { w.boolean(p_v); }
	void str(std::string &p_v) { w.str(p_v); }
	template <class E>
	void enumeration(E &p_v, size_t) {
		uint8_t b = uint8_t(p_v);
		w.u8(b);
	}
	void count(size_t &p_n, size_t, size_t) { w.u32(uint32_t(p_n)); }
	bool ok() const { return true; }
};

class In {
public:
	static constexpr bool kReading = true;
	explicit In(std::span<const uint8_t> p_blob) :
			r(p_blob) {}
	core::BlobReader r;
	void f32(float &r_v) {
		r_v = r.f32();
		if (r.ok() && (!std::isfinite(r_v) || std::fabs(r_v) > kMaxAbsParameter)) {
			r.fail("float not finite or out of range");
		}
	}
	void u8(uint8_t &r_v) { r_v = r.u8(); }
	void u16(uint16_t &r_v) { r_v = r.u16(); }
	void u32(uint32_t &r_v) { r_v = r.u32(); }
	void i16(int16_t &r_v) { r_v = int16_t(r.u16()); }
	void i32(int32_t &r_v) { r_v = r.i32(); }
	void boolean(bool &r_v) { r_v = r.boolean(); }
	void str(std::string &r_v) { r_v = r.str(kMaxNameLength); }
	template <class E>
	void enumeration(E &r_v, size_t p_count) {
		const uint8_t b = r.u8();
		if (r.ok() && b >= p_count) {
			r.fail("enum value " + std::to_string(b));
		}
		r_v = E(r.ok() ? b : 0);
	}
	void count(size_t &r_n, size_t p_min_bytes, size_t p_max) {
		const uint32_t n = r.u32();
		if (r.ok() && (n > p_max || size_t(n) > r.remaining() / (p_min_bytes == 0 ? 1 : p_min_bytes))) {
			r.fail("count " + std::to_string(n) + " past the bound or the bytes left");
		}
		r_n = r.ok() ? n : 0;
	}
	bool ok() const { return r.ok(); }
};

template <class IO>
void io_vec2(IO &io, Vec2 &v) {
	io.f32(v.x);
	io.f32(v.y);
}
template <class IO>
void io_vec3(IO &io, Vec3 &v) {
	io.f32(v.x);
	io.f32(v.y);
	io.f32(v.z);
}
template <class IO>
void io_vec4(IO &io, Vec4 &v) {
	io.f32(v.x);
	io.f32(v.y);
	io.f32(v.z);
	io.f32(v.w);
}
template <class IO>
void io_transform(IO &io, Transform &t) {
	io_vec3(io, t.right);
	io_vec3(io, t.up);
	io_vec3(io, t.forward);
	io_vec3(io, t.trans);
}
template <class IO>
void io_poly(IO &io, PolynomialDef &p) {
	io_vec4(io, p.coefficients);
	io.f32(p.min_clamp);
	io.f32(p.max_clamp);
	io.f32(p.scale);
}

template <class IO, class T, class F>
void io_vector(IO &io, std::vector<T> &v, size_t p_min_bytes, size_t p_max, F &&p_each) {
	size_t n = v.size();
	io.count(n, p_min_bytes, p_max);
	if constexpr (IO::kReading) {
		v.clear();
		if (!io.ok()) {
			return;
		}
		v.resize(n);
	}
	for (size_t i = 0; i < v.size() && io.ok(); ++i) {
		p_each(v[i]);
	}
}

template <class IO>
void io_evaluator(IO &io, EvaluatorDef &e) {
	io.enumeration(e.type, kEvaluatorTypeCount);
	io.enumeration(e.parameter, kEmitterParameterCount);
	if constexpr (IO::kReading) {

		switch (e.type) {
			case EvaluatorType::Polynomial:
				e.polynomial = PolynomialDef{};
				break;
			case EvaluatorType::PolynomialColorInterp:
				e.color_interp = ColorInterpDef{};
				break;
			case EvaluatorType::PolynomialOperator:
				e.op = OperatorDef{};
				break;
			case EvaluatorType::Random:
				e.random = RandomDef{};
				break;
			case EvaluatorType::RandomXyz:
				e.random_xyz = RandomXyzDef{};
				break;
			case EvaluatorType::Box:
				e.box = BoxDef{};
				break;
			case EvaluatorType::Sphere:
				e.sphere = SphereDef{};
				break;
			case EvaluatorType::RotateVector:
				e.rotate_vector = RotateVectorDef{};
				break;
			case EvaluatorType::CameraProximity:
				e.camera_proximity = CameraProximityDef{};
				break;
			case EvaluatorType::SampleTexture:
				e.sample_texture = SampleTextureDef{};
				break;
		}
	}
	switch (e.type) {
		case EvaluatorType::Polynomial:
			io_poly(io, e.polynomial);
			break;
		case EvaluatorType::PolynomialColorInterp:
			io_vec4(io, e.color_interp.coefficients);
			io_vec3(io, e.color_interp.color0);
			io_vec3(io, e.color_interp.color1);
			break;
		case EvaluatorType::PolynomialOperator:
			io_poly(io, e.op.first);
			io_poly(io, e.op.second);
			io.enumeration(e.op.operation, 2);
			io.f32(e.op.min_result);
			io.f32(e.op.max_result);
			break;
		case EvaluatorType::Random:
			io.f32(e.random.min);
			io.f32(e.random.max);
			break;
		case EvaluatorType::RandomXyz:
			io_vec3(io, e.random_xyz.min);
			io_vec3(io, e.random_xyz.max);
			break;
		case EvaluatorType::Box:
			io_vec3(io, e.box.dimensions);
			io_vec3(io, e.box.pivot);
			break;
		case EvaluatorType::Sphere:
			io.f32(e.sphere.radius);
			io_vec3(io, e.sphere.scale);
			io_vec3(io, e.sphere.pivot);
			break;
		case EvaluatorType::RotateVector:
			io.f32(e.rotate_vector.angle_degrees);
			io.boolean(e.rotate_vector.input_affects_phi);
			io.boolean(e.rotate_vector.rotate_within_plane);
			break;
		case EvaluatorType::CameraProximity:
			io.f32(e.camera_proximity.forward_offset);
			io.f32(e.camera_proximity.inner_radius);
			io_vec3(io, e.camera_proximity.inner_radius_direction);
			io_vec3(io, e.camera_proximity.offset);
			io_vec3(io, e.camera_proximity.size);
			break;
		case EvaluatorType::SampleTexture:
			io.u32(e.sample_texture.first);
			io.u32(e.sample_texture.count);
			io_vec3(io, e.sample_texture.intensity_min);
			io_vec3(io, e.sample_texture.intensity_max);
			io_vec2(io, e.sample_texture.dimensions);
			io_vec2(io, e.sample_texture.origin);
			break;
	}
}

template <class IO>
void io_params(IO &io, ProcessorParams &p) {
	io.f32(p.spawn_rate);
	io.boolean(p.distribute_over_time);
	io.boolean(p.distribute_over_distance);
	io.f32(p.trail_segment_length);
	io.f32(p.direction_from_emitter_origin);
	io.boolean(p.inherit_speed_and_direction);
	io.f32(p.speed);
	io.f32(p.size);
	io.f32(p.rotation);
	io.f32(p.rotation_speed);
	io.i32(p.animation_frame);
	io.f32(p.animation_speed);
	io.f32(p.particle_lifetime);
	io.f32(p.proximity_forward_offset);
	io_vec3(io, p.proximity_size);
	io.f32(p.drag);
	io.f32(p.gravity);
	io_vec3(io, p.local_force);
	io.f32(p.turbulence);
	io_vec2(io, p.size_pivot);
	io_vec3(io, p.color);
	io.f32(p.cull_threshold);
	io.f32(p.alpha_min_level);
	io.f32(p.alpha_max_level);
	io.f32(p.alpha_exponent);
	io.f32(p.texture_blend);
}

template <class IO>
void io_template(IO &io, EmitterTemplateDef &t) {
	io.enumeration(t.emittable, kEmittableTypeCount);
	io.f32(t.lifetime);
	io.u32(t.lifetime_frame_count);
	io.u32(t.max_count);
	io.f32(t.time_scale);
	io.u32(t.flags);
	io.str(t.mesh);
	io.f32(t.light.radius);
	io_vec3(io, t.light.color);
	io_poly(io, t.light.intensity);
	io.f32(t.light.random_min);
	io.f32(t.light.random_max);
	io_vec3(io, t.light.pivot);
	io.f32(t.max_spawn_distance);
	io.f32(t.visible_after_distance);
	io.f32(t.mesh_culling_distance);
	io.f32(t.min_screen_area);
	io.f32(t.particle_culling_factor);
	io.f32(t.distance_scale_near);
	io.f32(t.distance_scale_far);
	io.f32(t.distance_scale_length);
	io.f32(t.soft_particles_fade);
	io.f32(t.light_multiplier);
	io.f32(t.light_wrap_around);
	io.f32(t.vertex_pixel_lighting_blend);
	io.f32(t.global_local_normal_blend);
	io_vector(io, t.z_occlusion_lookup, 4, kMaxZOcclusionLookup, [&](float &r_f) { io.f32(r_f); });
}

template <class IO>
void io_emitter(IO &io, EmitterDef &e) {
	io.str(e.name);
	io_template(io, e.tmpl);
	for (ProcessorSlot &s : e.slots) {
		io.boolean(s.present);
		io.enumeration(s.input, kEvaluatorInputCount);
		io_vec4(io, s.default_values);
		io.i16(s.evaluator);
	}
	io_params(io, e.params);
	io_vector(io, e.evaluators, 2, kProcessorTypeCount, [&](EvaluatorDef &r_ev) { io_evaluator(io, r_ev); });
	io_vector(io, e.gradients, 16, kMaxGradientSamples, [&](Vec4 &r_g) { io_vec4(io, r_g); });
	io_vector(io, e.clip_lookup, 2, kMaxClipLookup, [&](int16_t &r_c) { io.i16(r_c); });
	io.str(e.texture.texture);
	io.str(e.texture.normal_map);
	io.u16(e.texture.frame_count);
	io.u16(e.texture.frame_columns);
	io.boolean(e.texture.animated);
	io_vector(io, e.children, 4, kMaxChildrenPerEmitter, [&](uint32_t &r_c) { io.u32(r_c); });
	io.str(e.base);
}

template <class IO>
void io_effect(IO &io, EffectDef &d) {
	io.str(d.name);
	io_transform(io, d.transform);
	io.f32(d.cull_distance);
	io.f32(d.start_delay);
	io.u8(d.tiers);
	io.u16(d.max_instance_count);
	io.boolean(d.reset_instance_when_started);
	io_vector(io, d.components, 60, kMaxEffectComponents, [&](EffectComponentDef &r_c) {
		io.enumeration(r_c.kind, 3);
		io_transform(io, r_c.transform);
		io.f32(r_c.spawn_probability);
		io.f32(r_c.start_delay);
		io.f32(r_c.cull_distance);
		io.u8(r_c.tiers);
		io.u16(r_c.max_instance_count);
		io.boolean(r_c.reset_instance_when_started);
		io.u32(r_c.emitter);
		io.str(r_c.asset);
	});
	io_vector(io, d.emitters, 64, kMaxEmittersPerEffect, [&](EmitterDef &r_e) { io_emitter(io, r_e); });
	io.u32(d.max_particles);
}

std::string effect_key_name(std::string_view p_blueprint) {
	return detail::canonical_name(p_blueprint);
}

}

std::vector<uint8_t> encode_effect_def(const EffectDef &p_def) {
	Out o;
	o.w.magic(kEffectMagic, kFxBlobFormatVersion);
	io_effect(o, const_cast<EffectDef &>(p_def));
	return o.w.take();
}

core::Result<EffectDef> decode_effect_def(std::span<const uint8_t> p_blob) {
	In in(p_blob);
	const uint32_t version = in.r.magic(kEffectMagic);
	if (in.ok() && version != kFxBlobFormatVersion) {
		return core::make_error("effect blob: format version " + std::to_string(version));
	}
	EffectDef def;
	io_effect(in, def);
	if (in.ok() && !in.r.at_end()) {
		in.r.fail("trailing bytes");
	}
	if (!in.ok()) {
		return in.r.error("effect blob");
	}
	for (const EmitterDef &e : def.emitters) {
		if ((e.tmpl.flags & ~kKnownEmitterFlags) != 0) {
			return core::make_error("effect blob: unknown emitter flag bits");
		}
		for (size_t i = 0; i < kProcessorTypeCount; ++i) {
			const ProcessorSlot &s = e.slots[i];
			if (!s.present && (s.evaluator != kNoEvaluator || s.input != EvaluatorInput::NormTime || !(s.default_values == Vec4{ 1, 1, 1, 1 }))) {
				return core::make_error("effect blob: an absent slot carries values");
			}
		}
	}
	const core::Status s = detail::validate_effect_def(def);
	if (!s.ok()) {
		return core::make_error("effect blob: " + s.error().message);
	}
	return def;
}

std::vector<uint8_t> encode_effect_deps(const std::vector<std::string> &p_sources) {
	core::BlobWriter w;
	w.magic(kDepsMagic, kFxDepsFormatVersion);
	w.u32(uint32_t(p_sources.size()));
	for (const std::string &s : p_sources) {
		w.str(s);
	}
	return w.take();
}

core::Result<std::vector<std::string>> decode_effect_deps(std::span<const uint8_t> p_blob) {
	core::BlobReader r(p_blob);
	const uint32_t version = r.magic(kDepsMagic);
	if (r.ok() && version != kFxDepsFormatVersion) {
		return core::make_error("fx deps blob: format version " + std::to_string(version));
	}
	const uint32_t n = r.u32();
	if (r.ok() && (n > kMaxDeps || n > r.remaining() / 4)) {
		r.fail("count " + std::to_string(n));
	}
	std::vector<std::string> out;
	if (r.ok()) {
		out.reserve(n);
	}
	for (uint32_t i = 0; i < n && r.ok(); ++i) {
		std::string s = r.str(kMaxNameLength);
		if (r.ok() && !out.empty() && !(out.back() < s)) {
			r.fail("dependencies not sorted and unique");
		}
		out.push_back(std::move(s));
	}
	if (r.ok() && !r.at_end()) {
		r.fail("trailing bytes");
	}
	if (!r.ok()) {
		return r.error("fx deps blob");
	}
	return out;
}

std::optional<core::CacheKey> effect_deps_key(const FxSources &p_sources, std::string_view p_blueprint) {
	if (!p_sources.digest) {
		return std::nullopt;
	}
	const std::optional<core::Sha1> root = p_sources.digest(effect_key_name(p_blueprint));
	if (!root) {
		return std::nullopt;
	}
	core::CacheKeyInputs in;
	in.kind = kFxDepsCacheKind;
	in.sources.push_back(core::source_digest(*root));
	in.converter_version = kFxConverterVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kFxDepsFormatVersion;
	in.params = "effect=" + effect_key_name(p_blueprint);
	return core::make_cache_key(in);
}

std::optional<core::CacheKey> effect_def_key(const FxSources &p_sources, std::string_view p_blueprint, const std::vector<std::string> &p_deps) {
	if (!p_sources.digest) {
		return std::nullopt;
	}
	core::CacheKeyInputs in;
	in.kind = kFxEffectCacheKind;
	for (const std::string &d : p_deps) {
		const std::optional<core::Sha1> sha1 = p_sources.digest(d);
		in.sources.push_back(sha1 ? core::source_digest(*sha1) : core::Sha1Digest{});
	}
	in.converter_version = kFxConverterVersion;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kFxBlobFormatVersion;
	in.params = "effect=" + effect_key_name(p_blueprint) + ";deps=" + core::sha256_of(encode_effect_deps(p_deps)).hex();
	return core::make_cache_key(in);
}

core::Result<CachedEffect> compile_effect_cached(core::Cache *p_cache, const FxSources &p_sources, std::string_view p_blueprint,
		core::ConvertTotals *r_totals) {
	CachedEffect out;
	const auto fail = [&](core::Error p_error) -> core::Result<CachedEffect> {
		if (r_totals != nullptr) {
			r_totals->add_failure();
		}
		return p_error;
	};
	const auto done = [&]() -> core::Result<CachedEffect> {
		if (r_totals != nullptr) {
			r_totals->add(out.item);
		}
		return std::move(out);
	};
	if (p_sources.db == nullptr) {
		return fail(core::make_error("compile_effect_cached: no resolver"));
	}
	const bool use_cache = p_cache != nullptr && p_cache->is_open();
	const std::optional<core::CacheKey> deps_key = use_cache ? effect_deps_key(p_sources, p_blueprint) : std::nullopt;
	std::optional<std::vector<std::string>> cached_deps;
	bool deps_dropped = false;
	if (deps_key) {
		core::GetResult got = p_cache->get(*deps_key);
		if (got.status == core::CacheStatus::Ok) {
			core::Result<std::vector<std::string>> d = decode_effect_deps(got.entry.payload());
			if (d) {
				cached_deps = std::move(*d);
			} else {
				got.entry.reset();
				p_cache->remove(*deps_key);
				deps_dropped = true;
			}
		} else if (got.dropped_corrupt) {
			deps_dropped = true;
		}
	}
	if (cached_deps) {
		if (const std::optional<core::CacheKey> key = effect_def_key(p_sources, p_blueprint, *cached_deps)) {
			core::GetResult got = p_cache->get(*key);
			if (got.status == core::CacheStatus::Ok) {
				core::Result<EffectDef> def = decode_effect_def(got.entry.payload());
				if (def) {
					out.def = std::move(*def);
					out.item.outcome = core::CacheOutcome::Hit;
					out.item.payload_bytes = got.entry.payload().size();
					out.dependencies = cached_deps->size();
					return done();
				}
				got.entry.reset();
				p_cache->remove(*key);
				out.item.dropped_corrupt = true;
			} else if (got.dropped_corrupt) {
				out.item.dropped_corrupt = true;
			}
		}
	}

	CompileContext ctx;
	ctx.db = p_sources.db;
	core::Result<EffectDef> def = compile_effect(p_blueprint, ctx);
	if (!def) {
		return fail(def.error());
	}
	const std::vector<std::string> &deps = ctx.sources;
	out.dependencies = deps.size();
	const std::vector<uint8_t> blob = encode_effect_def(*def);
	out.item.payload_bytes = blob.size();
	out.item.outcome = core::CacheOutcome::Uncached;
	out.item.dropped_corrupt = out.item.dropped_corrupt || deps_dropped;
	out.def = std::move(*def);
	if (!use_cache) {
		return done();
	}
	const std::optional<core::CacheKey> key = effect_def_key(p_sources, p_blueprint, deps);
	if (!deps_key || !key) {
		out.item.cache_error = "no manifest SHA1 for the blueprint";
		return done();
	}
	if (cached_deps && *cached_deps != deps) {
		p_cache->remove(*deps_key);
	}
	if (!cached_deps || *cached_deps != deps) {
		const core::PutResult put = p_cache->put(*deps_key, encode_effect_deps(deps));
		if (put.status != core::CacheStatus::Ok) {
			out.item.cache_error = std::string("put deps: ") + core::cache_status_name(put.status);
			return done();
		}
	}
	const core::PutResult put = p_cache->put(*key, blob);
	if (put.status == core::CacheStatus::Ok) {
		out.item.outcome = put.written ? core::CacheOutcome::Converted : core::CacheOutcome::Reused;
	} else {
		out.item.cache_error = std::string("put: ") + core::cache_status_name(put.status);
	}
	return done();
}

}
