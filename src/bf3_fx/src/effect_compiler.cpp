#include "bf3/fx/effect_compiler.h"

#include "bf3/core/thread_pool.h"
#include "bf3/core/util/format.h"
#include "fx_ebx.h"
#include "fx_internal.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace bf3::fx {

using core::EbxInstance;
using core::EbxStruct;
using core::EbxValue;
using detail::Reader;

namespace detail {

bool is_identity(const Transform &p_t) {
	const Transform id;
	const auto near = [](const Vec3 &p_a, const Vec3 &p_b) {
		return std::fabs(p_a.x - p_b.x) <= 1e-6f && std::fabs(p_a.y - p_b.y) <= 1e-6f && std::fabs(p_a.z - p_b.z) <= 1e-6f;
	};
	return near(p_t.right, id.right) && near(p_t.up, id.up) && near(p_t.forward, id.forward) && near(p_t.trans, id.trans);
}

namespace {

float value_bound(const EmitterDef &p_e, const ProcessorSlot &p_slot) {
	if (p_slot.evaluator == kNoEvaluator || size_t(p_slot.evaluator) >= p_e.evaluators.size()) {
		return p_slot.default_values.x;
	}
	const EvaluatorDef &ev = p_e.evaluators[size_t(p_slot.evaluator)];
	switch (ev.type) {
		case EvaluatorType::Random:
			return std::fmax(ev.random.min, ev.random.max);
		case EvaluatorType::RandomXyz:
			return std::fmax(ev.random_xyz.min.x, ev.random_xyz.max.x);
		case EvaluatorType::Polynomial:
			return std::fmax(ev.polynomial.min_clamp, ev.polynomial.max_clamp) * std::fabs(ev.polynomial.scale);
		case EvaluatorType::PolynomialOperator:
			return std::fmax(ev.op.min_result, ev.op.max_result);
		default:
			return 1.0f;
	}
}

bool finite_bounded(float p_v) {
	return std::isfinite(p_v) && std::fabs(p_v) <= kMaxAbsParameter;
}

}

float emitter_life_bound(const EmitterDef &p_e) {
	const ProcessorSlot &age = p_e.slot(ProcessorType::UpdateAge);
	if (!age.present) {
		return 0.0f;
	}
	return std::fmax(0.0f, p_e.params.particle_lifetime * value_bound(p_e, age));
}

uint64_t effect_particle_bound(const EffectDef &p_def) {
	const uint64_t cap = uint64_t(kMaxEffectParticles) + 1;

	std::vector<uint64_t> memo(p_def.emitters.size(), UINT64_MAX);
	std::function<uint64_t(uint32_t, uint32_t)> particles = [&](uint32_t p_e, uint32_t p_depth) -> uint64_t {
		if (p_e >= p_def.emitters.size() || p_depth > kMaxChildDepth + 1) {
			return cap;
		}
		if (memo[p_e] != UINT64_MAX) {
			return memo[p_e];
		}
		const EmitterDef &e = p_def.emitters[p_e];
		uint64_t inner = 1;
		for (const uint32_t c : e.children) {
			inner = std::min(cap, inner + particles(c, p_depth + 1));
		}
		const uint64_t out = std::min(cap, uint64_t(e.tmpl.max_count) * inner);
		memo[p_e] = out;
		return out;
	};
	uint64_t total = 0;
	for (const EffectComponentDef &c : p_def.components) {
		if (c.kind == ComponentKind::Emitter) {
			total = std::min(cap, total + particles(c.emitter, 0));
		}
	}
	return total;
}

core::Status validate_effect_def(const EffectDef &p_def) {
	const auto bad = [&](const std::string &p_why) { return core::Status(core::make_error(p_def.name + ": " + p_why)); };
	if (p_def.name.size() > kMaxNameLength) {
		return bad("name too long");
	}
	if (p_def.components.size() > kMaxEffectComponents) {
		return bad("too many components");
	}
	if (p_def.emitters.size() > kMaxEmittersPerEffect) {
		return bad("too many emitters");
	}
	if (p_def.max_instance_count < 1 || p_def.max_instance_count > kMaxInstanceCount) {
		return bad("MaxInstanceCount out of range");
	}
	if ((p_def.tiers & ~kTierAll) != 0) {
		return bad("tier bits");
	}
	const auto vec_ok = [](const Vec3 &p_v) { return finite_bounded(p_v.x) && finite_bounded(p_v.y) && finite_bounded(p_v.z); };
	const auto xf_ok = [&](const Transform &p_t) { return vec_ok(p_t.right) && vec_ok(p_t.up) && vec_ok(p_t.forward) && vec_ok(p_t.trans); };
	if (!xf_ok(p_def.transform) || !finite_bounded(p_def.cull_distance) || !finite_bounded(p_def.start_delay) || p_def.start_delay < 0) {
		return bad("effect transform, cull distance or start delay");
	}
	for (const EffectComponentDef &c : p_def.components) {
		if (uint8_t(c.kind) > uint8_t(ComponentKind::VisualEnvironment)) {
			return bad("component kind");
		}
		if (c.kind == ComponentKind::Emitter && c.emitter >= p_def.emitters.size()) {
			return bad("component emitter index");
		}
		if (c.kind == ComponentKind::Emitter && (p_def.emitters[c.emitter].tmpl.flags & kEmitterChild) != 0) {
			return bad("component uses a child emitter");
		}
		if (!xf_ok(c.transform) || !finite_bounded(c.spawn_probability) || !finite_bounded(c.start_delay) || c.start_delay < 0 ||
				!finite_bounded(c.cull_distance) || (c.tiers & ~kTierAll) != 0 || c.max_instance_count < 1 || c.max_instance_count > kMaxInstanceCount ||
				c.asset.size() > kMaxNameLength) {
			return bad("component fields");
		}
	}

	std::vector<uint8_t> state(p_def.emitters.size(), 0);
	std::function<bool(uint32_t, uint32_t)> dfs = [&](uint32_t p_e, uint32_t p_depth) -> bool {
		if (p_depth > kMaxChildDepth) {
			return false;
		}
		if (state[p_e] == 1) {
			return false;
		}
		state[p_e] = 1;
		for (const uint32_t c : p_def.emitters[p_e].children) {
			if (c >= p_def.emitters.size() || (p_def.emitters[c].tmpl.flags & kEmitterChild) == 0 || !dfs(c, p_depth + 1)) {
				return false;
			}
		}
		state[p_e] = 0;
		return true;
	};
	for (uint32_t e = 0; e < p_def.emitters.size(); ++e) {
		const EmitterDef &em = p_def.emitters[e];
		if (em.children.size() > kMaxChildrenPerEmitter) {
			return bad("too many children");
		}
		if ((em.tmpl.flags & kEmitterChild) == 0 && !dfs(e, 0)) {
			return bad("child emitter graph cyclic or deeper than " + std::to_string(kMaxChildDepth));
		}
	}
	for (uint32_t e = 0; e < p_def.emitters.size(); ++e) {
		const EmitterDef &em = p_def.emitters[e];
		const EmitterTemplateDef &t = em.tmpl;
		if (em.name.size() > kMaxNameLength || em.base.size() > kMaxNameLength || t.mesh.size() > kMaxNameLength ||
				em.texture.texture.size() > kMaxNameLength || em.texture.normal_map.size() > kMaxNameLength) {
			return bad("emitter name too long");
		}
		if (uint8_t(t.emittable) >= kEmittableTypeCount || t.max_count < 1 || t.max_count > kMaxEmitterMaxCount || !finite_bounded(t.lifetime) ||
				t.lifetime < 0 || !finite_bounded(t.time_scale) || t.time_scale <= 0) {
			return bad("emitter " + em.name + ": template fields");
		}
		if (em.texture.frame_count < 1 || em.texture.frame_count > kMaxFlipbookFrames || em.texture.frame_columns < 1 ||
				em.texture.frame_columns > em.texture.frame_count) {
			return bad("emitter " + em.name + ": flipbook counts");
		}
		if (t.z_occlusion_lookup.size() > kMaxZOcclusionLookup || em.clip_lookup.size() > kMaxClipLookup || em.gradients.size() > kMaxGradientSamples ||
				em.evaluators.size() > kProcessorTypeCount) {
			return bad("emitter " + em.name + ": table sizes");
		}
		const PointLightDef &l = t.light;
		const float floats[] = { t.max_spawn_distance, t.visible_after_distance, t.mesh_culling_distance, t.min_screen_area, t.particle_culling_factor,
			t.distance_scale_near, t.distance_scale_far, t.distance_scale_length, t.soft_particles_fade, t.light_multiplier, t.light_wrap_around,
			t.vertex_pixel_lighting_blend, t.global_local_normal_blend, l.radius, l.color.x, l.color.y, l.color.z, l.intensity.coefficients.x,
			l.intensity.coefficients.y, l.intensity.coefficients.z, l.intensity.coefficients.w, l.intensity.min_clamp, l.intensity.max_clamp,
			l.intensity.scale, l.random_min, l.random_max, l.pivot.x, l.pivot.y, l.pivot.z };
		for (const float f : floats) {
			if (!finite_bounded(f)) {
				return bad("emitter " + em.name + ": template float");
			}
		}
		for (const float f : t.z_occlusion_lookup) {
			if (!finite_bounded(f)) {
				return bad("emitter " + em.name + ": ZOcclusionLookup value");
			}
		}
		for (const ProcessorSlot &s : em.slots) {
			if (uint8_t(s.input) >= kEvaluatorInputCount || (s.evaluator != kNoEvaluator && (s.evaluator < 0 || size_t(s.evaluator) >= em.evaluators.size())) ||
					!finite_bounded(s.default_values.x) || !finite_bounded(s.default_values.y) || !finite_bounded(s.default_values.z) ||
					!finite_bounded(s.default_values.w)) {
				return bad("emitter " + em.name + ": processor slot");
			}
		}
		for (const EvaluatorDef &ev : em.evaluators) {
			if (uint8_t(ev.type) >= kEvaluatorTypeCount || uint8_t(ev.parameter) >= kEmitterParameterCount) {
				return bad("emitter " + em.name + ": evaluator type");
			}
			if (ev.type == EvaluatorType::SampleTexture &&
					(ev.sample_texture.first > em.gradients.size() || ev.sample_texture.count > em.gradients.size() - ev.sample_texture.first)) {
				return bad("emitter " + em.name + ": gradient range");
			}
			if (ev.type == EvaluatorType::PolynomialOperator && uint8_t(ev.op.operation) > uint8_t(PolynomialOperation::Addition)) {
				return bad("emitter " + em.name + ": operator");
			}
		}
		for (const Vec4 &g : em.gradients) {
			if (!finite_bounded(g.x) || !finite_bounded(g.y) || !finite_bounded(g.z) || !finite_bounded(g.w)) {
				return bad("emitter " + em.name + ": gradient value");
			}
		}
	}
	const uint64_t bound = effect_particle_bound(p_def);
	if (bound > kMaxEffectParticles) {
		return bad("particle bound " + std::to_string(bound) + " above " + std::to_string(kMaxEffectParticles));
	}
	if (bound != p_def.max_particles) {
		return bad("max_particles " + std::to_string(p_def.max_particles) + " differs from the bound " + std::to_string(bound));
	}
	return core::Status::success();
}

}

namespace {

struct Compile {
	Reader &r;
	CompileContext &ctx;
	std::set<std::string> unknown_types;
	std::set<std::string> unknown_enums;

	void unknown_type(std::string_view p_name) {
		unknown_types.insert(std::string(p_name));
		r.fail("unknown type " + std::string(p_name));
	}
	void unknown_enum(std::string_view p_field, std::string_view p_name) {
		unknown_enums.insert(std::string(p_field) + "=" + std::string(p_name));
		r.fail("unknown enumerator " + std::string(p_field) + " = " + std::string(p_name));
	}

	template <class E, class F>
	E enumerator(const EbxStruct &p_s, std::string_view p_field, F &&p_lookup, E p_default) {
		const std::string name = r.enum_name(p_s, p_field);
		if (!r.ok()) {
			return p_default;
		}
		const std::optional<E> e = p_lookup(name);
		if (!e) {
			unknown_enum(p_field, name);
			return p_default;
		}
		return *e;
	}

	PolynomialDef polynomial(const EbxStruct &p_s) {
		PolynomialDef p;
		p.coefficients = r.vec4(p_s, "Coefficients");
		p.min_clamp = r.number(p_s, "MinClamp");
		p.max_clamp = r.number(p_s, "MaxClamp");
		p.scale = r.number(p_s, "ScaleValue");
		return p;
	}

	int16_t evaluator(const EbxInstance &p_inst, EmitterDef &r_e) {
		if (!r.charge()) {
			return kNoEvaluator;
		}
		const std::string_view type_name = p_inst.type_name();
		const std::optional<EvaluatorType> type = evaluator_type_from_name(type_name);
		if (!type) {
			if (processor_type_from_name(type_name)) {
				r.fail("Pre names the processor " + std::string(type_name));
			} else {
				unknown_type(type_name);
			}
			return kNoEvaluator;
		}
		const EbxStruct s = p_inst.data();
		EvaluatorDef ev;
		ev.type = *type;
		ev.parameter = enumerator(s, "EmitterParameter", emitter_parameter_from_name, EmitterParameter::None);
		switch (*type) {
			case EvaluatorType::Polynomial:
				ev.polynomial = polynomial(s);
				break;
			case EvaluatorType::PolynomialColorInterp:
				ev.color_interp = ColorInterpDef{};
				ev.color_interp.coefficients = r.vec4(s, "Coefficients");
				ev.color_interp.color0 = r.vec3(s, "Color0");
				ev.color_interp.color1 = r.vec3(s, "Color1");
				break;
			case EvaluatorType::PolynomialOperator: {
				ev.op = OperatorDef{};
				if (const std::optional<EbxStruct> a = r.inline_struct(s, "FirstOperand")) {
					ev.op.first = polynomial(*a);
				}
				if (const std::optional<EbxStruct> b = r.inline_struct(s, "SecondOperand")) {
					ev.op.second = polynomial(*b);
				}
				ev.op.operation = enumerator(s, "Operation", polynomial_operation_from_name, PolynomialOperation::Multiplication);
				ev.op.min_result = r.number(s, "MinClampResult");
				ev.op.max_result = r.number(s, "MaxClampResult");
				break;
			}
			case EvaluatorType::Random:
				ev.random = RandomDef{ r.number(s, "Min"), r.number(s, "Max") };
				break;
			case EvaluatorType::RandomXyz:
				ev.random_xyz = RandomXyzDef{};
				ev.random_xyz.min = Vec3{ r.number(s, "MinX"), r.number(s, "MinY"), r.number(s, "MinZ") };
				ev.random_xyz.max = Vec3{ r.number(s, "MaxX"), r.number(s, "MaxY"), r.number(s, "MaxZ") };
				break;
			case EvaluatorType::Box:
				ev.box = BoxDef{ r.vec3(s, "Dimensions"), r.vec3(s, "Pivot") };
				break;
			case EvaluatorType::Sphere:
				ev.sphere = SphereDef{};
				ev.sphere.radius = r.number(s, "Radius");
				ev.sphere.scale = r.vec3(s, "Scale");
				ev.sphere.pivot = r.vec3(s, "Pivot");
				break;
			case EvaluatorType::RotateVector:
				ev.rotate_vector = RotateVectorDef{};
				ev.rotate_vector.angle_degrees = r.number(s, "Angle");
				ev.rotate_vector.input_affects_phi = r.flag(s, "InputAffectsPhi");
				ev.rotate_vector.rotate_within_plane = r.flag(s, "RotateWithinPlane");
				break;
			case EvaluatorType::CameraProximity:
				ev.camera_proximity = CameraProximityDef{};
				ev.camera_proximity.forward_offset = r.number(s, "ForwardOffset");
				ev.camera_proximity.inner_radius = r.number(s, "InnerRadius");
				ev.camera_proximity.inner_radius_direction = r.vec3(s, "InnerRadiusDirection");
				ev.camera_proximity.offset = r.vec3(s, "Offset");
				ev.camera_proximity.size = r.vec3(s, "Size");
				break;
			case EvaluatorType::SampleTexture: {
				ev.sample_texture = SampleTextureDef{};
				SampleTextureDef &st = ev.sample_texture;
				st.intensity_min = r.vec3(s, "ColorIntensityMin");
				st.intensity_max = r.vec3(s, "ColorIntensityMax");
				st.dimensions = r.vec2(s, "TextureDimensions");
				st.origin = Vec2{ r.number(s, "TextureOriginU"), r.number(s, "TextureOriginV") };
				const core::OptionalEbxArray rows = r.array(s, "GradientData", kMaxGradientSamples);
				if (rows && r_e.gradients.size() + rows->size() > kMaxGradientSamples) {
					r.fail("gradient rows past " + std::to_string(kMaxGradientSamples));
				}
				if (rows && r.ok()) {
					st.first = uint32_t(r_e.gradients.size());
					st.count = uint32_t(rows->size());
					for (const EbxValue row : *rows) {
						const std::optional<EbxStruct> v = row.as_struct();
						if (!v) {
							r.fail("GradientData element is not a struct");
							break;
						}
						r_e.gradients.push_back(Vec4{ r.number(*v, "x"), r.number(*v, "y"), r.number(*v, "z"), r.number(*v, "w") });
					}
				}
				break;
			}
		}
		if (!r.ok()) {
			return kNoEvaluator;
		}
		if (ev.parameter != EmitterParameter::None) {
			r_e.tmpl.flags |= kEmitterHasParameterisedEvaluator;
			++ctx.stats.parameterised_evaluators;
		}
		++ctx.stats.evaluators;
		r_e.evaluators.push_back(ev);
		return int16_t(r_e.evaluators.size() - 1);
	}

	void texture_info(const EbxStruct &p_s, EmitterDef &r_e) {
		const std::optional<EbxStruct> ti = r.inline_struct(p_s, "TextureInfo1");
		if (!ti) {
			return;
		}
		TextureRef &t = r_e.texture;
		t.texture = detail::canonical_name(r.text(*ti, "TextureName"));
		t.normal_map = detail::canonical_name(r.text(*ti, "NormalmapName"));
		t.animated = r.flag(*ti, "EnableAnimation");
		const float frames = r.number(*ti, "AnimationFrameCount");
		const float columns = r.number(*ti, "AnimationFrameColumnCount");
		if (!r.ok()) {
			return;
		}
		if (frames != std::floor(frames) || columns != std::floor(columns) || frames < 0 || columns < 0 || frames > float(kMaxFlipbookFrames) ||
				columns > frames) {
			r.fail("flipbook AnimationFrameCount " + std::to_string(frames) + " / AnimationFrameColumnCount " + std::to_string(columns) +
					" not integral with 0 <= columns <= frames <= " + std::to_string(kMaxFlipbookFrames));
			return;
		}
		if (frames == 0 || columns == 0) {
			++ctx.stats.flipbooks_normalised;
		}
		t.frame_count = uint16_t(frames == 0 ? 1 : frames);
		t.frame_columns = uint16_t(columns == 0 ? 1 : columns);
		if (t.frame_columns > t.frame_count) {
			t.frame_columns = t.frame_count;
		}
	}

	void processor(const EbxInstance &p_inst, ProcessorType p_type, EmitterDef &r_e) {
		const EbxStruct s = p_inst.data();
		ProcessorParams &pp = r_e.params;
		if (p_type == ProcessorType::BaseEmitter) {

			(void)r.flag(s, "Enable");
			r_e.base = r.external_name(s, "EmitterAsset");
			return;
		}
		if (!r.flag(s, "Enable")) {
			++ctx.stats.disabled_processors;
			return;
		}
		ProcessorSlot slot;
		slot.present = true;
		slot.input = enumerator(s, "EvaluatorInput", evaluator_input_from_name, EvaluatorInput::NormTime);
		slot.default_values = r.vec4(s, "DefaultValues");
		if (const std::optional<EbxInstance> pre = r.follow_internal(s, "Pre")) {
			slot.evaluator = evaluator(*pre, r_e);
		}
		switch (p_type) {
			case ProcessorType::Emitter: {
				const core::OptionalEbxArray assets = r.array(s, "EmitterAssets", kMaxChildrenPerEmitter);
				if (assets) {
					for (const EbxValue a : *assets) {
						const std::optional<core::EbxRef> ref = a.as_ref();
						if (!ref) {
							r.fail("EmitterData.EmitterAssets element is not a reference");
							break;
						}
						if (ref->is_null()) {
							continue;
						}
						pending_children.push_back(r.external_name_of(s, *ref, "EmitterAssets"));
					}
				}
				break;
			}
			case ProcessorType::SpawnRate:
				pp.spawn_rate = r.number(s, "SpawnRate");
				pp.distribute_over_time = r.flag(s, "DistributeOverTime");
				pp.distribute_over_distance = r.flag(s, "DistributeOverDistance");
				pp.trail_segment_length = r.number(s, "TrailSegmentLength");
				break;
			case ProcessorType::SpawnDirection:
				pp.direction_from_emitter_origin = r.number(s, "DirectionFromEmitterOrigin");
				pp.inherit_speed_and_direction = r.flag(s, "InheritSpeedAndDirectionFromEmitter");
				break;
			case ProcessorType::SpawnSpeed:
				pp.speed = r.number(s, "Speed");
				break;
			case ProcessorType::SpawnSize:
				pp.size = r.number(s, "Size");
				break;
			case ProcessorType::SpawnRotation:
				pp.rotation = r.number(s, "Rotation");
				break;
			case ProcessorType::SpawnRotationSpeed:
				pp.rotation_speed = r.number(s, "RotationSpeed");
				break;
			case ProcessorType::SpawnAnimationFrame:
				pp.animation_frame = int32_t(r.integer(s, "AnimationFrame", -int64_t(kMaxFlipbookFrames), int64_t(kMaxFlipbookFrames)));
				break;
			case ProcessorType::SpawnAnimation:
				pp.animation_speed = r.number(s, "AnimationSpeed");
				break;
			case ProcessorType::UpdateAge:
				pp.particle_lifetime = r.number(s, "Lifetime");
				break;
			case ProcessorType::UpdateCameraProximity:
				pp.proximity_forward_offset = r.number(s, "ForwardOffset");
				pp.proximity_size = r.vec3(s, "Size");
				break;
			case ProcessorType::AirResistance:
				pp.drag = r.number(s, "DragFactor");
				break;
			case ProcessorType::Gravity:
				pp.gravity = r.number(s, "Gravity");
				break;
			case ProcessorType::LocalForce:
				pp.local_force = r.vec3(s, "LocalForce");
				break;
			case ProcessorType::Turbulance:
				pp.turbulence = r.number(s, "Intensity");
				break;
			case ProcessorType::UpdateSize:
				pp.size_pivot = r.vec2(s, "Pivot");
				break;
			case ProcessorType::UpdateColor:
				pp.color = r.vec3(s, "Color");
				break;
			case ProcessorType::UpdateTransparency:
				pp.cull_threshold = r.number(s, "CullThreshold");
				break;
			case ProcessorType::UpdateAlphaLevelMin:
				pp.alpha_min_level = r.number(s, "MinLevel");
				break;
			case ProcessorType::UpdateAlphaLevelMax:
				pp.alpha_max_level = r.number(s, "MaxLevel");
				break;
			case ProcessorType::UpdateAlphaLevelScale:
				pp.alpha_exponent = r.number(s, "Exponent");
				break;
			case ProcessorType::UpdateTextureCoords:
				pp.texture_blend = r.number(s, "TextureBlendValue");
				texture_info(s, r_e);
				break;
			case ProcessorType::UpdateClipScale: {
				const core::OptionalEbxArray lookup = r.array(s, "Lookup", kMaxClipLookup);
				if (lookup) {
					r_e.clip_lookup.reserve(lookup->size());
					for (const EbxValue v : *lookup) {
						const std::optional<int64_t> i = v.as_i64();
						if (!i || *i < INT16_MIN || *i > INT16_MAX) {
							r.fail("UpdateClipScaleData.Lookup element is not an int16");
							break;
						}
						r_e.clip_lookup.push_back(int16_t(*i));
					}
				}
				break;
			}
			default:
				break;
		}
		if (r.ok()) {
			r_e.slots[size_t(p_type)] = slot;
			++ctx.stats.processors;
		}
	}

	std::optional<EbxInstance> document(std::string_view p_name) {
		if (!r.charge()) {
			return std::nullopt;
		}
		const core::EbxPartition *p = r.db().partition_by_name(p_name);
		if (p == nullptr) {
			r.fail("emitter document " + std::string(p_name) + " not found");
			return std::nullopt;
		}
		if (!r.charge(1 + p->instance_count() / 8)) {
			return std::nullopt;
		}
		const std::vector<EbxInstance> docs = p->instances_of("EmitterDocument");
		if (docs.empty()) {
			r.fail(std::string(p_name) + ": no EmitterDocument");
			return std::nullopt;
		}
		return docs.front();
	}

	void chain(const EbxInstance &p_doc, std::string_view p_doc_name, EmitterDef &r_e, uint32_t p_base_depth) {
		std::vector<std::pair<EbxInstance, ProcessorType>> links;
		std::set<uint32_t> visited;
		uint32_t seen = 0;
		std::optional<EbxInstance> at = r.follow_internal(p_doc.data(), "RootProcessor");
		while (at && r.ok()) {
			if (!r.charge()) {
				return;
			}
			if (links.size() >= kMaxChainLength) {
				r.fail(std::string(p_doc_name) + ": chain longer than " + std::to_string(kMaxChainLength));
				return;
			}
			if (!visited.insert(at->index()).second) {
				r.fail(std::string(p_doc_name) + ": processor chain has a cycle");
				return;
			}
			const std::string_view type_name = at->type_name();
			const std::optional<ProcessorType> type = processor_type_from_name(type_name);
			if (!type) {
				if (evaluator_type_from_name(type_name)) {
					r.fail(std::string(p_doc_name) + ": NextProcessor names the evaluator " + std::string(type_name));
				} else {
					unknown_type(type_name);
				}
				return;
			}
			if ((seen & (1u << uint32_t(*type))) != 0) {
				r.fail(std::string(p_doc_name) + ": processor type " + std::string(type_name) + " twice in one chain");
				return;
			}
			seen |= 1u << uint32_t(*type);
			links.emplace_back(*at, *type);
			at = r.follow_internal(at->data(), "NextProcessor");
		}
		if (!r.ok()) {
			return;
		}
		if (links.size() == 1 && links.front().second == ProcessorType::BaseEmitter) {
			const EbxStruct s = links.front().first.data();
			(void)r.flag(s, "Enable");
			const std::string base = r.external_name(s, "EmitterAsset");
			if (!r.ok()) {
				return;
			}
			if (base.empty()) {
				r.fail(std::string(p_doc_name) + ": BaseEmitterData without an EmitterAsset");
				return;
			}
			if (p_base_depth + 1 > kMaxBaseDepth) {
				r.fail(std::string(p_doc_name) + ": base emitters nested deeper than " + std::to_string(kMaxBaseDepth));
				return;
			}
			const std::optional<EbxInstance> base_doc = document(base);
			if (!base_doc) {
				return;
			}
			chain(*base_doc, base, r_e, p_base_depth + 1);
			if (p_base_depth == 0) {
				r_e.base = base;
				r_e.tmpl.flags |= kEmitterDerived;
				++ctx.stats.derived_from_base;
			}
			return;
		}
		for (const auto &[inst, type] : links) {
			processor(inst, type, r_e);
			if (!r.ok()) {
				return;
			}
		}
	}

	void templ(const EbxStruct &p_s, EmitterTemplateDef &r_t) {
		r_t.emittable = enumerator(p_s, "EmittableType", emittable_type_from_name, EmittableType::ScreenAlignedQuad);
		r_t.lifetime = r.number(p_s, "Lifetime");
		r_t.lifetime_frame_count = uint32_t(r.integer(p_s, "LifetimeFrameCount", 0, INT32_MAX));
		r_t.max_count = uint32_t(r.integer(p_s, "MaxCount", 1, kMaxEmitterMaxCount));
		r_t.time_scale = r.number(p_s, "TimeScale");
		struct FlagField {
			const char *name;
			uint32_t bit;
		};
		static const FlagField kFlags[] = { { "LocalSpace", kEmitterLocalSpace }, { "FollowSpawnSource", kEmitterFollowSpawnSource },
			{ "ActAsPointLight", kEmitterActAsPointLight }, { "Emissive", kEmitterEmissive }, { "Opaque", kEmitterOpaque },
			{ "ForceFullRes", kEmitterForceFullRes }, { "ForceNiceSorting", kEmitterForceNiceSorting },
			{ "KillParticlesWithEmitter", kEmitterKillParticlesWithEmitter }, { "RepeatParticleSpawning", kEmitterRepeatParticleSpawning },
			{ "TransparencySunShadowEnable", kEmitterTransparencySunShadow }, { "ExclusionVolumeCullEnable", kEmitterExclusionVolumeCull } };
		for (const FlagField &f : kFlags) {
			if (r.flag(p_s, f.name)) {
				r_t.flags |= f.bit;
			}
		}
		r_t.mesh = r.external_name(p_s, "Mesh");
		PointLightDef &l = r_t.light;
		l.radius = r.number(p_s, "PointLightRadius");
		l.color = r.vec3(p_s, "PointLightColor");
		l.intensity.coefficients = r.vec4(p_s, "PointLightIntensity");
		l.intensity.min_clamp = r.number(p_s, "PointLightMinClamp");
		l.intensity.max_clamp = r.number(p_s, "PointLightMaxClamp");
		l.intensity.scale = 1.0f;
		l.random_min = r.number(p_s, "PointLightRandomIntensityMin");
		l.random_max = r.number(p_s, "PointLightRandomIntensityMax");
		l.pivot = r.vec3(p_s, "PointLightPivot");
		r_t.max_spawn_distance = r.number(p_s, "MaxSpawnDistance");
		r_t.visible_after_distance = r.number(p_s, "VisibleAfterDistance");
		r_t.mesh_culling_distance = r.number(p_s, "MeshCullingDistance");
		r_t.min_screen_area = r.number(p_s, "MinScreenArea");
		r_t.particle_culling_factor = r.number(p_s, "ParticleCullingFactor");
		r_t.distance_scale_near = r.number(p_s, "DistanceScaleNearValue");
		r_t.distance_scale_far = r.number(p_s, "DistanceScaleFarValue");
		r_t.distance_scale_length = r.number(p_s, "DistanceScaleLength");
		r_t.soft_particles_fade = r.number(p_s, "SoftParticlesFadeDistanceMultiplier");
		r_t.light_multiplier = r.number(p_s, "LightMultiplier");
		r_t.light_wrap_around = r.number(p_s, "LightWrapAroundFactor");
		r_t.vertex_pixel_lighting_blend = r.number(p_s, "VertexPixelLightingBlendFactor");
		r_t.global_local_normal_blend = r.number(p_s, "GlobalLocalNormalBlendFactor");
		if (const core::OptionalEbxArray z = r.array(p_s, "ZOcclusionLookup", kMaxZOcclusionLookup)) {
			r_t.z_occlusion_lookup.reserve(z->size());
			for (const EbxValue v : *z) {
				const std::optional<float> f = v.as_f32();
				if (!f) {
					r.fail("ZOcclusionLookup element is not a float");
					break;
				}
				r_t.z_occlusion_lookup.push_back(r.checked(*f, "ZOcclusionLookup"));
			}
		}
		if (r.ok() && r_t.lifetime < 0) {
			r.fail("EmitterTemplateData.Lifetime " + std::to_string(r_t.lifetime) + " is negative");
		}
		if (r.ok() && !(r_t.time_scale > 0)) {
			r.fail("EmitterTemplateData.TimeScale " + std::to_string(r_t.time_scale) + " is not positive");
		}
	}

	std::vector<std::string> pending_children;

	std::optional<EmitterDef> emitter(const std::string &p_doc, std::vector<std::string> &r_children) {
		const std::optional<EbxInstance> doc = document(p_doc);
		if (!doc) {
			return std::nullopt;
		}
		EmitterDef e;
		e.name = p_doc;
		const std::optional<EbxInstance> tmpl = r.follow_internal(doc->data(), "TemplateData");
		if (!r.ok()) {
			return std::nullopt;
		}
		if (!tmpl || tmpl->type_name() != "EmitterTemplateData") {
			r.fail(p_doc + ": TemplateData is not an EmitterTemplateData");
			return std::nullopt;
		}
		if (!r.charge()) {
			return std::nullopt;
		}
		templ(tmpl->data(), e.tmpl);
		pending_children.clear();
		chain(*doc, p_doc, e, 0);
		if (!r.ok()) {
			return std::nullopt;
		}
		r_children = std::move(pending_children);
		pending_children.clear();
		++ctx.stats.emitters;
		return e;
	}

	struct Builder {
		Compile &c;
		std::vector<EmitterDef> emitters;
		std::map<std::pair<std::string, bool>, uint32_t> index;
		std::vector<std::string> names;

		uint32_t add(const std::string &p_doc, bool p_child) {
			const auto key = std::make_pair(p_doc, p_child);
			const auto it = index.find(key);
			if (it != index.end()) {
				return it->second;
			}
			if (names.size() >= kMaxEmittersPerEffect) {
				c.r.fail("more than " + std::to_string(kMaxEmittersPerEffect) + " emitters");
				return 0;
			}
			const uint32_t i = uint32_t(names.size());
			index.emplace(key, i);
			names.push_back(p_doc);
			emitters.emplace_back();
			emitters.back().name = p_doc;
			if (p_child) {
				emitters.back().tmpl.flags |= kEmitterChild;
			}
			return i;
		}
		bool read_all() {
			for (size_t i = 0; i < names.size() && c.r.ok(); ++i) {
				const bool child = (emitters[i].tmpl.flags & kEmitterChild) != 0;
				std::vector<std::string> kids;
				std::optional<EmitterDef> e = c.emitter(names[i], kids);
				if (!e) {
					return false;
				}
				if (child) {
					e->tmpl.flags |= kEmitterChild;
				}
				for (const std::string &k : kids) {
					const uint32_t ci = add(k, true);
					if (!c.r.ok()) {
						return false;
					}
					e->children.push_back(ci);
				}
				emitters[i] = std::move(*e);
			}
			return c.r.ok();
		}
	};

	void finish(EffectDef &r_def) {
		const uint64_t bound = detail::effect_particle_bound(r_def);
		r_def.max_particles = uint32_t(std::min<uint64_t>(bound, uint64_t(kMaxEffectParticles) + 1));
		if (r.ok()) {
			const core::Status s = detail::validate_effect_def(r_def);
			if (!s.ok()) {
				r.fail(s.error().message);
			}
		}
	}

	std::optional<EffectDef> effect(const std::string &p_blueprint) {
		if (!r.charge()) {
			return std::nullopt;
		}
		const core::EbxPartition *bp = r.db().partition_by_name(p_blueprint);
		if (bp == nullptr) {
			r.fail("partition not found");
			return std::nullopt;
		}
		const EbxInstance primary = bp->primary();
		if (primary.type_name() != "EffectBlueprint") {
			r.fail("primary instance is " + std::string(primary.type_name()) + ", not an EffectBlueprint");
			return std::nullopt;
		}
		const std::optional<EbxInstance> ent = r.follow_internal(primary.data(), "Object");
		if (!r.ok()) {
			return std::nullopt;
		}
		if (!ent || ent->type_name() != "EffectEntityData") {
			r.fail("EffectBlueprint.Object is not an EffectEntityData");
			return std::nullopt;
		}
		const EbxStruct s = ent->data();
		EffectDef def;
		def.name = p_blueprint;
		def.transform = r.transform(s, "Transform");
		def.cull_distance = r.number(s, "CullDistance");
		def.start_delay = r.number(s, "StartDelay");
		def.tiers = tiers(s);
		def.max_instance_count = uint16_t(r.integer(s, "MaxInstanceCount", 1, kMaxInstanceCount));
		def.reset_instance_when_started = r.flag(s, "ResetInstanceWhenStarted");
		if (r.ok() && def.start_delay < 0) {
			r.fail("EffectEntityData.StartDelay is negative");
		}
		Builder b{ *this, {}, {}, {} };
		const core::OptionalEbxArray comps = r.array(s, "Components", kMaxEffectComponents);
		if (!r.ok()) {
			return std::nullopt;
		}
		for (const EbxValue v : *comps) {
			const std::optional<core::EbxRef> ref = v.as_ref();
			if (!ref) {
				r.fail("Components element is not a reference");
				return std::nullopt;
			}
			if (ref->is_null()) {
				continue;
			}
			if (ref->kind != core::EbxRef::Kind::Internal) {
				r.fail("Components element is an external reference");
				return std::nullopt;
			}
			if (!r.charge()) {
				return std::nullopt;
			}
			const std::optional<EbxInstance> ci = bp->resolve(*ref);
			if (!ci) {
				r.fail("Components element does not resolve");
				return std::nullopt;
			}
			const EbxStruct cs = ci->data();
			const std::string_view type = ci->type_name();
			EffectComponentDef c;
			if (type == "EmitterEntityData") {
				c.kind = ComponentKind::Emitter;
				const std::string doc = r.external_name(cs, "Emitter");
				if (r.ok() && doc.empty()) {
					r.fail("EmitterEntityData without an Emitter");
				}
				c.spawn_probability = r.number(cs, "SpawnProbability");
				c.max_instance_count = uint16_t(r.integer(cs, "MaxInstanceCount", 1, kMaxInstanceCount));
				c.reset_instance_when_started = r.flag(cs, "ResetInstanceWhenStarted");
				if (r.ok()) {
					c.emitter = b.add(doc, false);
				}
			} else if (type == "SoundEffectEntityData") {
				c.kind = ComponentKind::Sound;
				c.asset = r.external_name(cs, "Sound");
				++ctx.stats.sounds;
			} else if (type == "VisualEnvironmentEffectEntityData") {
				c.kind = ComponentKind::VisualEnvironment;
				c.asset = r.external_name(cs, "VisualEnvironment");
				++ctx.stats.visual_environments;
			} else {
				unknown_type(type);
				return std::nullopt;
			}
			c.transform = r.transform(cs, "Transform");
			c.start_delay = r.number(cs, "StartDelay");
			c.cull_distance = r.number(cs, "CullDistance");
			c.tiers = tiers(cs);
			if (r.ok() && c.start_delay < 0) {
				r.fail("component StartDelay is negative");
			}
			if (!r.ok()) {
				return std::nullopt;
			}
			def.components.push_back(std::move(c));
		}
		if (!b.read_all()) {
			return std::nullopt;
		}
		def.emitters = std::move(b.emitters);
		finish(def);
		if (!r.ok()) {
			return std::nullopt;
		}
		return def;
	}

	uint8_t tiers(const EbxStruct &p_s) {
		uint8_t t = 0;
		if (r.flag(p_s, "LowEndPc")) {
			t |= kTierLow;
		}
		if (r.flag(p_s, "MediumPc")) {
			t |= kTierMedium;
		}
		if (r.flag(p_s, "HighEndPc")) {
			t |= kTierHigh;
		}
		return t;
	}
};

void publish(Compile &p_c, const std::set<std::string> &p_sources) {
	CompileContext &ctx = p_c.ctx;
	ctx.sources.assign(p_sources.begin(), p_sources.end());
	for (const std::string &u : p_c.unknown_types) {
		if (std::find(ctx.unknown_types.begin(), ctx.unknown_types.end(), u) == ctx.unknown_types.end()) {
			ctx.unknown_types.push_back(u);
		}
	}
	for (const std::string &u : p_c.unknown_enums) {
		if (std::find(ctx.unknown_enumerators.begin(), ctx.unknown_enumerators.end(), u) == ctx.unknown_enumerators.end()) {
			ctx.unknown_enumerators.push_back(u);
		}
	}
}

core::Result<std::vector<EmitterDef>> compile_emitters(std::string_view p_document, CompileContext &r_ctx) {
	if (r_ctx.db == nullptr) {
		return core::make_error("compile_emitter: no resolver");
	}
	std::set<std::string> sources;
	const detail::RecordingResolver rec(*r_ctx.db, sources);
	Reader r(rec, r_ctx.budget);
	const std::string name = detail::canonical_name(p_document);
	r.set_where(name);
	Compile c{ r, r_ctx, {}, {} };
	Compile::Builder b{ c, {}, {}, {} };
	if (name.size() > kMaxNameLength) {
		r.fail("name too long");
	} else {
		b.add(name, false);
		b.read_all();
	}
	publish(c, sources);
	if (!r.ok()) {
		return r.error();
	}
	return std::move(b.emitters);
}

}

core::Result<std::vector<EmitterDef>> compile_emitter(std::string_view p_document, CompileContext &r_ctx) {
	return compile_emitters(p_document, r_ctx);
}

core::Result<EffectDef> compile_effect(std::string_view p_blueprint, CompileContext &r_ctx) {
	if (r_ctx.db == nullptr) {
		return core::make_error("compile_effect: no resolver");
	}
	std::set<std::string> sources;
	const detail::RecordingResolver rec(*r_ctx.db, sources);
	Reader r(rec, r_ctx.budget);
	const std::string name = detail::canonical_name(p_blueprint);
	r.set_where(name);
	Compile c{ r, r_ctx, {}, {} };
	std::optional<EffectDef> def;
	if (name.size() > kMaxNameLength) {
		r.fail("name too long");
	} else {
		def = c.effect(name);
	}
	publish(c, sources);
	if (!r.ok() || !def) {
		return r.ok() ? core::make_error(name + ": compile failed") : r.error();
	}
	return std::move(*def);
}

core::Result<EffectDef> compile_emitter_effect(std::string_view p_document, CompileContext &r_ctx) {
	core::Result<std::vector<EmitterDef>> emitters = compile_emitters(p_document, r_ctx);
	if (!emitters) {
		return emitters.error();
	}
	EffectDef def;
	def.name = detail::canonical_name(p_document);
	def.max_instance_count = 1;
	def.reset_instance_when_started = true;
	EffectComponentDef c;
	c.kind = ComponentKind::Emitter;
	c.emitter = 0;
	def.components.push_back(c);
	def.emitters = std::move(*emitters);
	const uint64_t bound = detail::effect_particle_bound(def);
	def.max_particles = uint32_t(std::min<uint64_t>(bound, uint64_t(kMaxEffectParticles) + 1));
	const core::Status s = detail::validate_effect_def(def);
	if (!s.ok()) {
		return s.error();
	}
	return def;
}

core::Result<std::string> find_partition(std::span<const std::string> p_catalogue, std::string_view p_name) {
	const std::string want = detail::canonical_name(p_name);
	if (want.empty()) {
		return core::make_error("find_partition: empty name");
	}
	std::vector<std::string> hits;
	for (const std::string &c : p_catalogue) {
		const std::string have = detail::canonical_name(c);
		if (have == want) {
			return c;
		}
		if (have.size() > want.size() && have.compare(have.size() - want.size(), want.size(), want) == 0 && have[have.size() - want.size() - 1] == '/') {
			hits.push_back(c);
		}
	}
	if (hits.size() == 1) {
		return hits.front();
	}
	std::string msg = hits.empty() ? "no partition named " + std::string(p_name) : std::to_string(hits.size()) + " partitions end with /" + want + ":";
	for (size_t i = 0; i < hits.size() && i < 8; ++i) {
		msg += " " + hits[i];
	}
	return core::make_error(msg);
}

EffectExtent effect_extent(const EffectDef &p_def) {
	EffectExtent out;
	std::function<void(uint32_t, float, uint32_t)> visit = [&](uint32_t p_e, float p_start, uint32_t p_depth) {
		if (p_e >= p_def.emitters.size() || p_depth > kMaxChildDepth) {
			return;
		}
		const EmitterDef &e = p_def.emitters[p_e];
		const float ts = e.tmpl.time_scale > 0 ? e.tmpl.time_scale : 1.0f;
		if (e.tmpl.lifetime <= 0 || (e.tmpl.flags & kEmitterRepeatParticleSpawning) != 0) {
			out.finite = false;
		}
		const float spawn_end = p_start + e.tmpl.lifetime / ts;
		const float end = spawn_end + detail::emitter_life_bound(e) / ts;
		out.spawn_end = std::fmax(out.spawn_end, spawn_end);
		out.end = std::fmax(out.end, end);
		for (const uint32_t c : e.children) {
			visit(c, spawn_end, p_depth + 1);
		}
	};
	for (const EffectComponentDef &c : p_def.components) {
		const float start = p_def.start_delay + c.start_delay;
		out.spawn_end = std::fmax(out.spawn_end, start);
		out.end = std::fmax(out.end, start);
		if (c.kind == ComponentKind::Emitter) {
			visit(c.emitter, start, 0);
		}
	}
	if (!out.finite) {
		out.spawn_end = INFINITY;
		out.end = INFINITY;
	}
	return out;
}

core::Result<CensusReport> run_census(const core::EbxLibrary &p_library, unsigned p_threads) {
	const auto start = std::chrono::steady_clock::now();
	CensusReport rep;
	std::vector<std::string> blueprints;
	std::vector<std::string> documents;
	for (const core::EbxLibrary::Entry &e : p_library.entries()) {
		const core::EbxInstance primary = e.partition->primary();
		if (primary.type_name() == "EffectBlueprint") {
			blueprints.emplace_back(e.name);
		} else if (!e.partition->instances_of("EmitterDocument").empty()) {
			documents.emplace_back(e.name);
		}
	}
	rep.blueprints = blueprints.size();
	rep.emitter_documents = documents.size();
	struct Item {
		bool ok = false;
		std::string error;
		std::vector<std::string> unknown_types;
		std::vector<std::string> unknown_enumerators;
		size_t work = 0;

		size_t emitter_entities = 0;
		size_t dual_role = 0;
		bool transform = false;
		size_t component_pools = 0;

		uint32_t processor_bits = 0;
		uint32_t evaluator_bits = 0;
		size_t flipbooks_normalised = 0;
	};
	const size_t n = blueprints.size() + documents.size();
	std::vector<Item> items(n);
	core::parallel_for(
			n,
			[&](size_t i) {
				Item &it = items[i];
				CompileContext ctx;
				ctx.db = &p_library;
				if (i < blueprints.size()) {
					core::Result<EffectDef> d = compile_effect(blueprints[i], ctx);
					it.ok = d.ok();
					if (d) {
						std::set<std::string> roots;
						std::set<std::string> kids;
						for (const EffectComponentDef &c : d->components) {
							if (c.kind == ComponentKind::Emitter) {
								++it.emitter_entities;
							}
							if (c.max_instance_count != 1 || !c.reset_instance_when_started) {
								++it.component_pools;
							}
						}
						for (const EmitterDef &e : d->emitters) {
							((e.tmpl.flags & kEmitterChild) != 0 ? kids : roots).insert(e.name);
						}
						for (const std::string &k : kids) {
							it.dual_role += roots.count(k);
						}
						it.transform = !detail::is_identity(d->transform);
					} else {
						it.error = d.error().message;
					}
				} else {
					core::Result<std::vector<EmitterDef>> d = compile_emitter(documents[i - blueprints.size()], ctx);
					it.ok = d.ok();
					if (d) {
						const EmitterDef &e = d->front();
						for (size_t t = 0; t < kProcessorTypeCount; ++t) {
							if (e.slots[t].present) {
								it.processor_bits |= 1u << t;
							}
						}
						if (!e.base.empty()) {
							it.processor_bits |= 1u << uint32_t(ProcessorType::BaseEmitter);
						}
						if (!e.children.empty()) {
							it.processor_bits |= 1u << uint32_t(ProcessorType::Emitter);
						}
						for (const EvaluatorDef &ev : e.evaluators) {
							it.evaluator_bits |= 1u << uint32_t(ev.type);
						}
						it.flipbooks_normalised = ctx.stats.flipbooks_normalised;
					} else {
						it.error = d.error().message;
					}
				}
				it.unknown_types = std::move(ctx.unknown_types);
				it.unknown_enumerators = std::move(ctx.unknown_enumerators);
				it.work = ctx.budget.used();
			},
			p_threads);
	std::set<std::string> ut;
	std::set<std::string> ue;
	for (size_t i = 0; i < n; ++i) {
		const Item &it = items[i];
		if (it.ok) {
			(i < blueprints.size() ? rep.blueprints_compiled : rep.emitter_documents_compiled) += 1;
		} else if (rep.errors.size() < 32) {
			rep.errors.push_back(it.error);
		}
		ut.insert(it.unknown_types.begin(), it.unknown_types.end());
		ue.insert(it.unknown_enumerators.begin(), it.unknown_enumerators.end());
		rep.max_work_units = std::max(rep.max_work_units, it.work);
		rep.emitter_entities += it.emitter_entities;
		rep.dual_role_documents += it.dual_role;
		rep.effects_with_transform += it.transform ? 1 : 0;
		rep.component_pools += it.component_pools;
		rep.flipbooks_normalised += it.flipbooks_normalised;
		for (size_t t = 0; t < kProcessorTypeCount; ++t) {
			rep.processor_emitters[t] += (it.processor_bits >> t) & 1u;
		}
		for (size_t t = 0; t < kEvaluatorTypeCount; ++t) {
			rep.evaluator_emitters[t] += (it.evaluator_bits >> t) & 1u;
		}
	}
	rep.unknown_types.assign(ut.begin(), ut.end());
	rep.unknown_enumerators.assign(ue.begin(), ue.end());
	rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return rep;
}

namespace {

std::string g(float p_v) {
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%.9g", double(p_v));
	return buf;
}
std::string v2(const Vec2 &p_v) {
	return "(" + g(p_v.x) + " " + g(p_v.y) + ")";
}
std::string v3(const Vec3 &p_v) {
	return "(" + g(p_v.x) + " " + g(p_v.y) + " " + g(p_v.z) + ")";
}
std::string v4(const Vec4 &p_v) {
	return "(" + g(p_v.x) + " " + g(p_v.y) + " " + g(p_v.z) + " " + g(p_v.w) + ")";
}
std::string xf(const Transform &p_t) {
	return "right " + v3(p_t.right) + " up " + v3(p_t.up) + " forward " + v3(p_t.forward) + " trans " + v3(p_t.trans);
}
std::string poly(const PolynomialDef &p_p) {
	return "coefficients " + v4(p_p.coefficients) + " clamp [" + g(p_p.min_clamp) + " " + g(p_p.max_clamp) + "] scale " + g(p_p.scale);
}

std::string evaluator_text(const EvaluatorDef &p_e) {
	std::string s = std::string(evaluator_type_name(p_e.type)) + " parameter " + std::string(emitter_parameter_name(p_e.parameter));
	switch (p_e.type) {
		case EvaluatorType::Polynomial:
			return s + " " + poly(p_e.polynomial);
		case EvaluatorType::PolynomialColorInterp:
			return s + " coefficients " + v4(p_e.color_interp.coefficients) + " color0 " + v3(p_e.color_interp.color0) + " color1 " +
					v3(p_e.color_interp.color1);
		case EvaluatorType::PolynomialOperator:
			return s + " op " + (p_e.op.operation == PolynomialOperation::Multiplication ? "mul" : "add") + " first {" + poly(p_e.op.first) +
					"} second {" + poly(p_e.op.second) + "} result [" + g(p_e.op.min_result) + " " + g(p_e.op.max_result) + "]";
		case EvaluatorType::Random:
			return s + " min " + g(p_e.random.min) + " max " + g(p_e.random.max);
		case EvaluatorType::RandomXyz:
			return s + " min " + v3(p_e.random_xyz.min) + " max " + v3(p_e.random_xyz.max);
		case EvaluatorType::Box:
			return s + " dimensions " + v3(p_e.box.dimensions) + " pivot " + v3(p_e.box.pivot);
		case EvaluatorType::Sphere:
			return s + " radius " + g(p_e.sphere.radius) + " scale " + v3(p_e.sphere.scale) + " pivot " + v3(p_e.sphere.pivot);
		case EvaluatorType::RotateVector:
			return s + " angle " + g(p_e.rotate_vector.angle_degrees) + " phi " + std::to_string(int(p_e.rotate_vector.input_affects_phi)) + " plane " +
					std::to_string(int(p_e.rotate_vector.rotate_within_plane));
		case EvaluatorType::CameraProximity:
			return s + " forward " + g(p_e.camera_proximity.forward_offset) + " inner " + g(p_e.camera_proximity.inner_radius) + " direction " +
					v3(p_e.camera_proximity.inner_radius_direction) + " offset " + v3(p_e.camera_proximity.offset) + " size " + v3(p_e.camera_proximity.size);
		case EvaluatorType::SampleTexture:
			return s + " rows " + std::to_string(p_e.sample_texture.first) + "+" + std::to_string(p_e.sample_texture.count) + " min " +
					v3(p_e.sample_texture.intensity_min) + " max " + v3(p_e.sample_texture.intensity_max) + " dimensions " +
					v2(p_e.sample_texture.dimensions) + " origin " + v2(p_e.sample_texture.origin);
	}
	return s;
}

}

std::string format_emitter_def(const EmitterDef &p_e) {
	std::string o;
	const EmitterTemplateDef &t = p_e.tmpl;
	o += core::fmt("emitter %s\n", p_e.name.c_str());
	o += core::fmt("  base %s flags 0x%08x\n", p_e.base.empty() ? "-" : p_e.base.c_str(), t.flags);
	o += "  template " + std::string(emittable_type_name(t.emittable)) + " lifetime " + g(t.lifetime) + " frames " + std::to_string(t.lifetime_frame_count) +
			" max_count " + std::to_string(t.max_count) + " time_scale " + g(t.time_scale) + " mesh " + (t.mesh.empty() ? "-" : t.mesh) + "\n";
	o += "  light radius " + g(t.light.radius) + " color " + v3(t.light.color) + " intensity {" + poly(t.light.intensity) + "} random [" +
			g(t.light.random_min) + " " + g(t.light.random_max) + "] pivot " + v3(t.light.pivot) + "\n";
	o += "  render spawn_distance " + g(t.max_spawn_distance) + " visible_after " + g(t.visible_after_distance) + " mesh_cull " +
			g(t.mesh_culling_distance) + " min_area " + g(t.min_screen_area) + " cull_factor " + g(t.particle_culling_factor) + " distance_scale (" +
			g(t.distance_scale_near) + " " + g(t.distance_scale_far) + " " + g(t.distance_scale_length) + ") soft " + g(t.soft_particles_fade) +
			" light " + g(t.light_multiplier) + " wrap " + g(t.light_wrap_around) + " vp_blend " + g(t.vertex_pixel_lighting_blend) + " gl_blend " +
			g(t.global_local_normal_blend) + "\n";
	o += "  z_occlusion " + std::to_string(t.z_occlusion_lookup.size());
	for (const float f : t.z_occlusion_lookup) {
		o += " " + g(f);
	}
	o += "\n";
	o += "  texture " + (p_e.texture.texture.empty() ? std::string("-") : p_e.texture.texture) + " normal " +
			(p_e.texture.normal_map.empty() ? std::string("-") : p_e.texture.normal_map) + " frames " + std::to_string(p_e.texture.frame_count) + "/" +
			std::to_string(p_e.texture.frame_columns) + " animated " + std::to_string(int(p_e.texture.animated)) + "\n";
	o += "  children";
	for (const uint32_t c : p_e.children) {
		o += " " + std::to_string(c);
	}
	o += "\n";
	const ProcessorParams &p = p_e.params;
	o += "  params rate " + g(p.spawn_rate) + " over_time " + std::to_string(int(p.distribute_over_time)) + " over_distance " +
			std::to_string(int(p.distribute_over_distance)) + " trail " + g(p.trail_segment_length) + " from_origin " + g(p.direction_from_emitter_origin) +
			" inherit " + std::to_string(int(p.inherit_speed_and_direction)) + " speed " + g(p.speed) + " size " + g(p.size) + " rotation " + g(p.rotation) +
			" rotation_speed " + g(p.rotation_speed) + " frame " + std::to_string(p.animation_frame) + " animation_speed " + g(p.animation_speed) +
			" life " + g(p.particle_lifetime) + " proximity " + g(p.proximity_forward_offset) + " " + v3(p.proximity_size) + " drag " + g(p.drag) +
			" gravity " + g(p.gravity) + " local_force " + v3(p.local_force) + " turbulence " + g(p.turbulence) + " size_pivot " + v2(p.size_pivot) +
			" color " + v3(p.color) + " cull " + g(p.cull_threshold) + " alpha (" + g(p.alpha_min_level) + " " + g(p.alpha_max_level) + " " +
			g(p.alpha_exponent) + ") blend " + g(p.texture_blend) + "\n";
	for (size_t i = 0; i < kProcessorTypeCount; ++i) {
		const ProcessorSlot &s = p_e.slots[i];
		if (!s.present) {
			continue;
		}
		o += "  slot " + std::string(processor_type_name(ProcessorType(i))) + " input " + std::string(evaluator_input_name(s.input)) + " default " +
				v4(s.default_values) + " pre " + std::to_string(s.evaluator) + "\n";
	}
	for (size_t i = 0; i < p_e.evaluators.size(); ++i) {
		o += "  evaluator " + std::to_string(i) + " " + evaluator_text(p_e.evaluators[i]) + "\n";
	}
	o += "  gradients " + std::to_string(p_e.gradients.size());
	for (const Vec4 &r : p_e.gradients) {
		o += " " + v4(r);
	}
	o += "\n  clip " + std::to_string(p_e.clip_lookup.size());
	for (const int16_t c : p_e.clip_lookup) {
		o += " " + std::to_string(c);
	}
	o += "\n";
	return o;
}

std::string format_effect_def(const EffectDef &p_def) {
	std::string o;
	o += core::fmt("effect %s\n", p_def.name.c_str());
	o += "transform " + xf(p_def.transform) + "\n";
	o += "cull_distance " + g(p_def.cull_distance) + " start_delay " + g(p_def.start_delay) + " tiers " + std::to_string(p_def.tiers) +
			" max_instance_count " + std::to_string(p_def.max_instance_count) + " reset " + std::to_string(int(p_def.reset_instance_when_started)) +
			" max_particles " + std::to_string(p_def.max_particles) + "\n";
	for (size_t i = 0; i < p_def.components.size(); ++i) {
		const EffectComponentDef &c = p_def.components[i];
		static const char *kKinds[] = { "emitter", "sound", "visual_environment" };
		o += "component " + std::to_string(i) + " " + kKinds[size_t(c.kind) < 3 ? size_t(c.kind) : 0] + " emitter " + std::to_string(c.emitter) + " asset " +
				(c.asset.empty() ? std::string("-") : c.asset) + " p " + g(c.spawn_probability) + " delay " + g(c.start_delay) + " cull " +
				g(c.cull_distance) + " tiers " + std::to_string(c.tiers) + " mic " + std::to_string(c.max_instance_count) + " reset " +
				std::to_string(int(c.reset_instance_when_started)) + " transform " + xf(c.transform) + "\n";
	}
	for (size_t i = 0; i < p_def.emitters.size(); ++i) {
		o += "[" + std::to_string(i) + "] " + format_emitter_def(p_def.emitters[i]);
	}
	return o;
}

}
