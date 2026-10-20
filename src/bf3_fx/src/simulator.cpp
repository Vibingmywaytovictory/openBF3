#include "bf3/fx/simulator.h"

#include "bf3/core/thread_pool.h"
#include "fx_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace bf3::fx {

namespace {

constexpr uint32_t kNone = UINT32_MAX;
constexpr size_t kChunk = 256;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTurbulenceKnotsPerSecond = 30.0f;

inline float clampf(float p_v, float p_lo, float p_hi) {

	p_v = p_v < p_lo ? p_lo : p_v;
	return p_v > p_hi ? p_hi : p_v;
}

inline Vec3 add(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a.x + p_b.x, p_a.y + p_b.y, p_a.z + p_b.z };
}
inline Vec3 mul(const Vec3 &p_a, float p_s) {
	return { p_a.x * p_s, p_a.y * p_s, p_a.z * p_s };
}
inline float dot(const Vec3 &p_a, const Vec3 &p_b) {
	return p_a.x * p_b.x + p_a.y * p_b.y + p_a.z * p_b.z;
}
inline Vec3 cross(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a.y * p_b.z - p_a.z * p_b.y, p_a.z * p_b.x - p_a.x * p_b.z, p_a.x * p_b.y - p_a.y * p_b.x };
}
inline float length(const Vec3 &p_v) {
	return std::sqrt(dot(p_v, p_v));
}
inline Vec3 normalize_or_zero(const Vec3 &p_v) {
	const float l = length(p_v);
	return l > 1e-20f ? mul(p_v, 1.0f / l) : Vec3{};
}

inline Vec3 rotate(const Transform &p_t, const Vec3 &p_v) {
	return { p_v.x * p_t.right.x + p_v.y * p_t.up.x + p_v.z * p_t.forward.x, p_v.x * p_t.right.y + p_v.y * p_t.up.y + p_v.z * p_t.forward.y,
		p_v.x * p_t.right.z + p_v.y * p_t.up.z + p_v.z * p_t.forward.z };
}
inline Vec3 unrotate(const Transform &p_t, const Vec3 &p_v) {
	return { dot(p_v, p_t.right), dot(p_v, p_t.up), dot(p_v, p_t.forward) };
}
inline Vec3 apply(const Transform &p_t, const Vec3 &p_p) {
	return add(p_t.trans, rotate(p_t, p_p));
}

Transform compose(const Transform &p_outer, const Transform &p_inner) {
	Transform t;
	t.right = rotate(p_outer, p_inner.right);
	t.up = rotate(p_outer, p_inner.up);
	t.forward = rotate(p_outer, p_inner.forward);
	t.trans = apply(p_outer, p_inner.trans);
	return t;
}

Transform frame_from_up(const Vec3 &p_up, const Vec3 &p_trans) {
	Transform t;
	t.up = p_up;
	const Vec3 helper = std::fabs(p_up.y) < 0.99f ? Vec3{ 0, 1, 0 } : Vec3{ 1, 0, 0 };
	t.right = normalize_or_zero(cross(helper, p_up));
	t.forward = cross(t.right, p_up);
	t.trans = p_trans;
	return t;
}

inline bool finite3(float p_a, float p_b, float p_c) {
	return std::isfinite(p_a) && std::isfinite(p_b) && std::isfinite(p_c);
}

inline float draw(uint64_t p_seed, uint32_t p_serial, uint32_t p_slot, uint32_t p_lane) {
	return fx_unit(fx_key(p_seed, p_serial, p_slot, p_lane));
}

Vec4 eval_one(const EmitterDef &p_e, const EvaluatorDef &p_ev, uint32_t p_slot, float p_input, uint64_t p_seed, uint32_t p_serial,
		const Vec4 &p_defaults) {
	switch (p_ev.type) {
		case EvaluatorType::Polynomial: {
			const float v = evaluate_polynomial(p_ev.polynomial, p_input);
			return { v, v, v, v };
		}
		case EvaluatorType::PolynomialColorInterp: {
			const Vec3 c = evaluate_color_interp(p_ev.color_interp.coefficients, p_ev.color_interp.color0, p_ev.color_interp.color1, p_input);
			return { c.x, c.y, c.z, 1.0f };
		}
		case EvaluatorType::PolynomialOperator: {
			const float a = evaluate_polynomial(p_ev.op.first, p_input);
			const float b = evaluate_polynomial(p_ev.op.second, p_input);
			const float v = clampf(p_ev.op.operation == PolynomialOperation::Multiplication ? a * b : a + b, p_ev.op.min_result, p_ev.op.max_result);
			return { v, v, v, v };
		}
		case EvaluatorType::Random: {
			const float v = fx_uniform(p_ev.random.min, p_ev.random.max, draw(p_seed, p_serial, p_slot, 0));
			return { v, v, v, v };
		}
		case EvaluatorType::RandomXyz:
			return { fx_uniform(p_ev.random_xyz.min.x, p_ev.random_xyz.max.x, draw(p_seed, p_serial, p_slot, 0)),
				fx_uniform(p_ev.random_xyz.min.y, p_ev.random_xyz.max.y, draw(p_seed, p_serial, p_slot, 1)),
				fx_uniform(p_ev.random_xyz.min.z, p_ev.random_xyz.max.z, draw(p_seed, p_serial, p_slot, 2)), 1.0f };
		case EvaluatorType::Box: {

			const BoxDef &b = p_ev.box;
			return { (0.5f * b.pivot.x + draw(p_seed, p_serial, p_slot, 0) - 0.5f) * b.dimensions.x,
				(0.5f * b.pivot.y + draw(p_seed, p_serial, p_slot, 1) - 0.5f) * b.dimensions.y,
				(0.5f * b.pivot.z + draw(p_seed, p_serial, p_slot, 2) - 0.5f) * b.dimensions.z, 1.0f };
		}
		case EvaluatorType::Sphere: {
			const SphereDef &s = p_ev.sphere;
			const float cos_theta = 2.0f * draw(p_seed, p_serial, p_slot, 0) - 1.0f;
			const float sin_theta = std::sqrt(std::fmax(0.0f, 1.0f - cos_theta * cos_theta));
			const float phi = 2.0f * kPi * draw(p_seed, p_serial, p_slot, 1);
			const float r = s.radius * std::cbrt(draw(p_seed, p_serial, p_slot, 2));
			return { s.pivot.x + s.scale.x * r * sin_theta * std::cos(phi), s.pivot.y + s.scale.y * r * cos_theta,
				s.pivot.z + s.scale.z * r * sin_theta * std::sin(phi), 1.0f };
		}
		case EvaluatorType::RotateVector: {

			const Vec3 axis_raw{ p_defaults.x, p_defaults.y, p_defaults.z };
			Vec3 axis = normalize_or_zero(axis_raw);
			if (axis.x == 0 && axis.y == 0 && axis.z == 0) {
				axis = { 0, 1, 0 };
			}
			const float angle = clampf(p_ev.rotate_vector.angle_degrees, 0.0f, 180.0f) * (kPi / 180.0f);
			const float cos_theta = 1.0f - draw(p_seed, p_serial, p_slot, 0) * (1.0f - std::cos(angle));
			const float sin_theta = std::sqrt(std::fmax(0.0f, 1.0f - cos_theta * cos_theta));
			const float phi = p_ev.rotate_vector.rotate_within_plane ? 0.0f : 2.0f * kPi * draw(p_seed, p_serial, p_slot, 1);
			const Transform f = frame_from_up(axis, {});
			const Vec3 d = add(add(mul(f.up, cos_theta), mul(f.right, sin_theta * std::cos(phi))), mul(f.forward, sin_theta * std::sin(phi)));
			const float len = length(axis_raw);
			return { d.x * len, d.y * len, d.z * len, 1.0f };
		}
		case EvaluatorType::CameraProximity:
			return p_defaults;
		case EvaluatorType::SampleTexture: {
			const SampleTextureDef &st = p_ev.sample_texture;
			Vec4 s{ 1, 1, 1, 1 };
			if (st.count > 0 && size_t(st.first) + st.count <= p_e.gradients.size()) {
				const float x = clampf(p_input, 0.0f, 1.0f) * float(st.count - 1);
				const uint32_t i0 = std::min(uint32_t(x), st.count - 1);
				const uint32_t i1 = std::min(i0 + 1, st.count - 1);
				const float f = x - float(i0);
				const Vec4 &a = p_e.gradients[st.first + i0];
				const Vec4 &b = p_e.gradients[st.first + i1];
				s = { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f };
			}
			return { st.intensity_min.x + (st.intensity_max.x - st.intensity_min.x) * s.x, st.intensity_min.y + (st.intensity_max.y - st.intensity_min.y) * s.y,
				st.intensity_min.z + (st.intensity_max.z - st.intensity_min.z) * s.z, s.w };
		}
	}
	return p_defaults;
}

struct Soa {
	static constexpr size_t kFloats = 19;

	std::array<std::vector<float>, kFloats> f;
	std::vector<uint32_t> serial;

	void allocate(size_t p_n) {
		for (std::vector<float> &v : f) {
			v.assign(p_n, 0.0f);
		}
		serial.assign(p_n, 0);
	}
	void copy(const Soa &p_src, size_t p_from, size_t p_to, size_t p_n) {
		if (p_n == 0) {
			return;
		}
		for (size_t k = 0; k < kFloats; ++k) {
			std::memcpy(f[k].data() + p_to, p_src.f[k].data() + p_from, p_n * sizeof(float));
		}
		std::memcpy(serial.data() + p_to, p_src.serial.data() + p_from, p_n * sizeof(uint32_t));
	}
	void move_one(size_t p_from, size_t p_to) {
		for (std::vector<float> &v : f) {
			v[p_to] = v[p_from];
		}
		serial[p_to] = serial[p_from];
	}
};
enum SoaField : size_t { kPx, kPy, kPz, kVx, kVy, kVz, kSx, kSy, kSz, kRot, kRspd, kFrame, kFrate, kAge, kLife, kAx, kAy, kAz, kLight };

}

struct FxWorld::Impl {
	struct EffectSlot {
		uint32_t generation = 0;
		bool active = false;
		bool killed = false;
		bool stopping = false;
		std::shared_ptr<const EffectDef> def;
		EffectStart start;
		Transform base;
		float age = 0;
		uint64_t sequence = 0;
		std::array<uint64_t, 4> selected{};
		std::array<uint64_t, 4> started{};
		uint32_t live_emitters = 0;
	};
	struct EmitterInstance {
		uint32_t generation = 0;
		bool active = false;
		uint32_t effect = 0;
		uint32_t emitter = 0;
		uint32_t component = 0;
		uint32_t parent = kNone;
		uint32_t parent_generation = 0;
		uint32_t parent_serial = 0;
		uint32_t parent_child_slot = 0;
		uint64_t seed = 0;
		float e_age = 0;
		float acc = 1;
		bool burst_pending = true;
		bool stopped = false;
		bool kill = false;
		Transform transform;
		Vec3 source_velocity;
		Vec3 prev_trans;
		bool has_prev = false;
		uint32_t state_first = 0;
		uint32_t state_count = 0;
		uint32_t next_serial = 0;
		std::array<uint16_t, kMaxChildrenPerEmitter> live_children{};
	};
	struct Item {
		uint32_t instance = 0;
		uint32_t old_first = 0, old_count = 0;
		uint32_t first = 0, capacity = 0;
		uint32_t light_first = 0;
		uint32_t spawn = 0;
		uint32_t first_serial = 0;
		float a0 = 0;
		float rate_dt = 0;
		float dt_e = 0;
		float te_spawn = 0;
		float te = 0;
		bool kill_all = false;
		bool window_over = false;

		uint32_t alive = 0;
		uint32_t lights = 0;
		uint32_t nonfinite = 0;
	};

	FxWorldConfig cfg;
	Soa buf[2];
	int cur = 0;
	std::vector<ParticleVertex> vertices;
	std::vector<ParticleLight> lights;
	std::vector<EmitterOutput> outputs;
	std::vector<FxEvent> events;
	size_t particle_out = 0, light_out = 0;
	FxStats stats;

	std::vector<EffectSlot> effects;
	std::vector<uint32_t> free_effects;
	std::vector<uint32_t> effect_list;
	std::vector<EmitterInstance> emitters;
	std::vector<uint32_t> free_emitters;
	std::vector<uint32_t> emitter_list;
	std::vector<Item> items;
	uint64_t sequence = 0;
	uint32_t live_particles = 0;

	uint32_t substeps = 0, substep = 0;
	float sub_dt = 0;

	explicit Impl(const FxWorldConfig &p_cfg) :
			cfg(p_cfg) {
		cfg.max_effects = std::max(1u, cfg.max_effects);
		cfg.max_emitter_instances = std::max(1u, cfg.max_emitter_instances);
		cfg.max_particles = std::max(1u, cfg.max_particles);
		if (!(cfg.max_substep > 0) || !std::isfinite(cfg.max_substep)) {
			cfg.max_substep = 0.25f;
		}
		if (!(cfg.max_dt > 0) || !std::isfinite(cfg.max_dt)) {
			cfg.max_dt = 2.0f;
		}
		buf[0].allocate(cfg.max_particles);
		buf[1].allocate(cfg.max_particles);
		vertices.assign(cfg.max_particles, ParticleVertex{});
		lights.assign(cfg.max_particles, ParticleLight{});
		outputs.reserve(cfg.max_emitter_instances);
		events.reserve(size_t(cfg.max_effects) * 4);
		effects.assign(cfg.max_effects, EffectSlot{});
		emitters.assign(cfg.max_emitter_instances, EmitterInstance{});
		effect_list.reserve(cfg.max_effects);
		emitter_list.reserve(cfg.max_emitter_instances);
		items.reserve(cfg.max_emitter_instances);
		free_effects.reserve(cfg.max_effects);
		free_emitters.reserve(cfg.max_emitter_instances);
		reset_free_lists();
	}

	void reset_free_lists() {
		free_effects.clear();
		for (uint32_t i = cfg.max_effects; i-- > 0;) {
			free_effects.push_back(i);
		}
		free_emitters.clear();
		for (uint32_t i = cfg.max_emitter_instances; i-- > 0;) {
			free_emitters.push_back(i);
		}
	}

	static bool bit(const std::array<uint64_t, 4> &p_bits, uint32_t p_i) { return (p_bits[p_i >> 6] >> (p_i & 63)) & 1u; }
	static void set_bit(std::array<uint64_t, 4> &r_bits, uint32_t p_i) { r_bits[p_i >> 6] |= uint64_t(1) << (p_i & 63); }

	void event(FxEvent::Kind p_kind, uint32_t p_effect, uint32_t p_component) {
		if (events.size() >= events.capacity()) {
			++stats.events_dropped;
			return;
		}
		const EffectSlot &s = effects[p_effect];
		FxEvent e;
		e.kind = p_kind;
		e.effect = EffectHandle{ p_effect, s.generation };
		e.component = p_component;
		e.def = s.def.get();
		e.transform = compose(s.base, s.def->components[p_component].transform);
		events.push_back(e);
	}

	uint32_t new_emitter(uint32_t p_effect, uint32_t p_emitter, uint32_t p_component, uint64_t p_seed) {
		if (free_emitters.empty()) {
			++stats.emitter_starts_dropped;
			return kNone;
		}
		const uint32_t id = free_emitters.back();
		free_emitters.pop_back();
		EmitterInstance &e = emitters[id];
		const uint32_t gen = e.generation + 1 == 0 ? 1 : e.generation + 1;
		e = EmitterInstance{};
		e.generation = gen;
		e.active = true;
		e.effect = p_effect;
		e.emitter = p_emitter;
		e.component = p_component;
		e.seed = p_seed;
		e.state_first = 0;
		e.state_count = 0;
		emitter_list.push_back(id);
		++effects[p_effect].live_emitters;
		return id;
	}

	void retire_emitter(uint32_t p_id) {
		EmitterInstance &e = emitters[p_id];
		if (e.parent != kNone) {
			EmitterInstance &p = emitters[e.parent];
			if (p.active && p.generation == e.parent_generation && p.live_children[e.parent_child_slot] > 0) {
				--p.live_children[e.parent_child_slot];
			}
		}
		live_particles -= std::min(live_particles, e.state_count);
		e.active = false;
		e.state_count = 0;
		--effects[e.effect].live_emitters;
		free_emitters.push_back(p_id);
	}

	void retire_effect(uint32_t p_slot) {
		EffectSlot &s = effects[p_slot];
		const EffectDef &d = *s.def;
		for (uint32_t c = 0; c < d.components.size(); ++c) {
			if (bit(s.started, c) && bit(s.selected, c)) {
				if (d.components[c].kind == ComponentKind::Sound) {
					event(FxEvent::Kind::SoundStop, p_slot, c);
				} else if (d.components[c].kind == ComponentKind::VisualEnvironment) {
					event(FxEvent::Kind::VisualEnvironmentStop, p_slot, c);
				}
			}
		}
		s.active = false;
		s.killed = false;
		s.def.reset();
		free_effects.push_back(p_slot);
	}

	void sweep() {
		size_t w = 0;
		for (size_t i = 0; i < emitter_list.size(); ++i) {
			const uint32_t id = emitter_list[i];
			if (emitters[id].active) {
				emitter_list[w++] = id;
			}
		}
		emitter_list.resize(w);
		w = 0;
		for (size_t i = 0; i < effect_list.size(); ++i) {
			const uint32_t slot = effect_list[i];
			EffectSlot &s = effects[slot];
			if (s.active && !s.killed && s.live_emitters == 0 && all_started(s)) {
				retire_effect(slot);
			}
			if (s.active) {
				effect_list[w++] = slot;
			}
		}
		effect_list.resize(w);
	}

	bool all_started(const EffectSlot &p_s) const {
		if (p_s.stopping) {
			return true;
		}
		const size_t n = p_s.def->components.size();
		for (uint32_t c = 0; c < n; ++c) {
			if (bit(p_s.selected, c) && !bit(p_s.started, c)) {
				return false;
			}
		}
		return true;
	}

	void retire_killed() {
		bool any = false;
		for (const uint32_t slot : effect_list) {
			if (effects[slot].killed) {
				any = true;
				break;
			}
		}
		if (!any) {
			return;
		}
		for (const uint32_t id : emitter_list) {
			EmitterInstance &e = emitters[id];
			if (e.active && effects[e.effect].killed) {
				retire_emitter(id);
			}
		}
		size_t w = 0;
		for (size_t i = 0; i < emitter_list.size(); ++i) {
			if (emitters[emitter_list[i]].active) {
				emitter_list[w++] = emitter_list[i];
			}
		}
		emitter_list.resize(w);
		w = 0;
		for (size_t i = 0; i < effect_list.size(); ++i) {
			const uint32_t slot = effect_list[i];
			if (effects[slot].killed) {
				retire_effect(slot);
			} else {
				effect_list[w++] = slot;
			}
		}
		effect_list.resize(w);
	}

	void start_components(uint32_t p_slot, float p_dt) {
		EffectSlot &s = effects[p_slot];
		const EffectDef &d = *s.def;
		if (!s.stopping) {
			for (uint32_t c = 0; c < d.components.size(); ++c) {
				if (!bit(s.selected, c) || bit(s.started, c)) {
					continue;
				}
				const EffectComponentDef &comp = d.components[c];
				const float delay = d.start_delay + comp.start_delay;
				if (s.age < delay) {
					continue;
				}
				set_bit(s.started, c);
				if (comp.kind == ComponentKind::Sound) {
					event(FxEvent::Kind::SoundStart, p_slot, c);
					continue;
				}
				if (comp.kind == ComponentKind::VisualEnvironment) {
					event(FxEvent::Kind::VisualEnvironmentStart, p_slot, c);
					continue;
				}
				const EmitterDef &e = d.emitters[comp.emitter];

				float e_age = 0;
				if (s.start.start_age > delay) {
					e_age = (s.start.start_age - delay) * e.tmpl.time_scale;
					const bool repeat = (e.tmpl.flags & kEmitterRepeatParticleSpawning) != 0;
					if (e.tmpl.lifetime > 0 && !repeat && e_age >= e.tmpl.lifetime) {
						continue;
					}
					if (repeat && e.tmpl.lifetime > 0) {
						e_age = std::fmod(e_age, e.tmpl.lifetime);
					}
				}
				const uint32_t id = new_emitter(p_slot, comp.emitter, c, fx_emitter_seed(s.start.seed, c, comp.emitter));
				if (id == kNone) {
					continue;
				}
				EmitterInstance &inst = emitters[id];
				inst.e_age = e_age;
				if (e_age > 0) {
					inst.acc = 0;
					inst.burst_pending = false;
				}
			}
		}
		s.age += p_dt;
	}

	Vec4 slot_value(const EmitterDef &p_e, ProcessorType p_type, float p_input, uint64_t p_seed, uint32_t p_serial) const {
		const ProcessorSlot &slot = p_e.slot(p_type);
		if (slot.evaluator == kNoEvaluator) {
			return slot.default_values;
		}
		return eval_one(p_e, p_e.evaluators[size_t(slot.evaluator)], uint32_t(p_type), p_input, p_seed, p_serial, slot.default_values);
	}

	static float input_of(EvaluatorInput p_input, float p_t, float p_te, float p_speed) {
		switch (p_input) {
			case EvaluatorInput::NormTime:
				return p_t;
			case EvaluatorInput::EmitterNormTime:
				return p_te;
			case EvaluatorInput::One:
				return 1.0f;
			case EvaluatorInput::Velocity:
				return p_speed;
		}
		return p_t;
	}

	void plan(uint32_t p_id, float p_dt, uint32_t &r_budget, uint32_t &r_offset, uint32_t &r_light_offset) {
		EmitterInstance &inst = emitters[p_id];
		EffectSlot &fx = effects[inst.effect];
		const EffectDef &d = *fx.def;
		const EmitterDef &e = d.emitters[inst.emitter];
		const EmitterTemplateDef &t = e.tmpl;
		const bool repeat = (t.flags & kEmitterRepeatParticleSpawning) != 0;
		if (fx.stopping) {
			inst.stopped = true;
		}

		if (inst.parent == kNone) {
			inst.transform = compose(fx.base, d.components[inst.component].transform);
			inst.source_velocity = fx.start.velocity;
		} else if (!inst.stopped) {
			const EmitterInstance &p = emitters[inst.parent];
			bool found = false;
			if (p.active && p.generation == inst.parent_generation && p.state_count > 0) {
				const Soa &s = buf[cur];
				const uint32_t *begin = s.serial.data() + p.state_first;
				const uint32_t *end = begin + p.state_count;
				const uint32_t *it = std::lower_bound(begin, end, inst.parent_serial);
				if (it != end && *it == inst.parent_serial) {
					const size_t k = size_t(it - s.serial.data());
					const EmitterDef &pe = d.emitters[p.emitter];
					Vec3 pos{ s.f[kPx][k], s.f[kPy][k], s.f[kPz][k] };
					Vec3 vel{ s.f[kVx][k], s.f[kVy][k], s.f[kVz][k] };
					if ((pe.tmpl.flags & kEmitterLocalSpace) != 0) {
						pos = apply(p.transform, pos);
						vel = rotate(p.transform, vel);
					}
					const Vec3 up = normalize_or_zero(vel);
					if (up.x == 0 && up.y == 0 && up.z == 0) {
						inst.transform = p.transform;
						inst.transform.trans = pos;
					} else {
						inst.transform = frame_from_up(up, pos);
					}
					inst.source_velocity = vel;
					found = true;
				}
			}
			if (!found) {
				inst.stopped = true;
			}
		}
		const float dt_e = p_dt * t.time_scale;
		Item it;
		it.instance = p_id;
		it.old_first = inst.state_first;
		it.old_count = inst.state_count;
		it.dt_e = dt_e;
		const float te_before = t.lifetime > 0 ? std::fmin(inst.e_age / t.lifetime, 1.0f) : 0.0f;
		it.te_spawn = te_before;
		it.kill_all = inst.kill || (inst.stopped && (t.flags & kEmitterKillParticlesWithEmitter) != 0);

		double due = 0;
		const bool window_open = !inst.stopped && (t.lifetime <= 0 || inst.e_age < t.lifetime);
		if (window_open && e.has(ProcessorType::SpawnRate)) {
			const Vec4 v = slot_value(e, ProcessorType::SpawnRate, input_of(e.slot(ProcessorType::SpawnRate).input, 0, te_before, 0), inst.seed, 0);
			const float rate = std::fmax(0.0f, e.params.spawn_rate * v.x);
			const bool burst = !e.params.distribute_over_time && t.lifetime > 0;
			if (burst) {
				if (inst.burst_pending) {
					due = std::floor(1.0 + double(rate) * double(t.lifetime));
					inst.burst_pending = false;
				}
			} else {
				it.a0 = inst.acc;
				double inc = 0;
				if (e.params.distribute_over_distance) {
					const Vec3 tr = inst.transform.trans;
					const float moved = inst.has_prev ? length(Vec3{ tr.x - inst.prev_trans.x, tr.y - inst.prev_trans.y, tr.z - inst.prev_trans.z }) : 0.0f;
					inc = double(rate) * double(std::isfinite(moved) ? moved : 0.0f);
				} else {
					const float window = t.lifetime > 0 ? std::fmin(dt_e, t.lifetime - inst.e_age) : dt_e;
					inc = double(rate) * double(std::fmax(0.0f, window));
					it.rate_dt = rate * dt_e;
				}
				const double total = double(inst.acc) + inc;
				due = std::floor(total);
				inst.acc = float(total - due);
			}
		}
		inst.prev_trans = inst.transform.trans;
		inst.has_prev = true;

		const uint32_t due_n = due >= double(UINT32_MAX) ? UINT32_MAX : uint32_t(due);
		const uint32_t room = t.max_count > inst.state_count ? t.max_count - inst.state_count : 0;
		const uint32_t capped = std::min(due_n, room);
		const uint32_t n = std::min(capped, r_budget);
		stats.spawns_capped += due_n - capped;
		stats.spawns_dropped += capped - n;
		r_budget -= n;
		it.spawn = n;
		it.first_serial = inst.next_serial;
		inst.next_serial += n;
		it.first = r_offset;
		it.capacity = inst.state_count + n;
		r_offset += it.capacity;
		if ((t.flags & kEmitterActAsPointLight) != 0) {
			it.light_first = r_light_offset;
			r_light_offset += it.capacity;
		} else {
			it.light_first = kNone;
		}

		inst.e_age += dt_e;
		if (repeat && t.lifetime > 0 && inst.e_age >= t.lifetime) {
			inst.e_age = std::fmod(inst.e_age, t.lifetime);
			inst.acc = 1.0f;
			inst.burst_pending = true;
		}
		it.te = t.lifetime > 0 ? std::fmin(inst.e_age / t.lifetime, 1.0f) : 0.0f;
		it.window_over = inst.stopped || (t.lifetime > 0 && !repeat && inst.e_age >= t.lifetime);
		items.push_back(it);
	}

	size_t begin_step() {
		if (substep >= substeps) {
			return 0;
		}
		items.clear();
		retire_killed();
		const float dt = sub_dt;
		for (size_t i = 0; i < effect_list.size(); ++i) {
			start_components(effect_list[i], dt);
		}
		uint32_t budget = cfg.max_particles - std::min(cfg.max_particles, live_particles);
		uint32_t offset = 0;
		uint32_t light_offset = 0;
		for (size_t i = 0; i < emitter_list.size(); ++i) {
			plan(emitter_list[i], dt, budget, offset, light_offset);
		}
		return items.size();
	}

	void run_item(Item &r_it) {
		const EmitterInstance &inst = emitters[r_it.instance];
		const EffectSlot &fx = effects[inst.effect];
		const EffectDef &d = *fx.def;
		const EmitterDef &e = d.emitters[inst.emitter];
		const EmitterTemplateDef &t = e.tmpl;
		const Soa &src = buf[cur];
		Soa &dst = buf[1 - cur];
		const bool local = (t.flags & kEmitterLocalSpace) != 0;
		const uint32_t base = r_it.first;
		dst.copy(src, r_it.old_first, base, r_it.old_count);

		for (uint32_t k = 0; k < r_it.spawn; ++k) {
			spawn_one(e, inst, r_it, k, dst, base + r_it.old_count + k, local);
		}
		const uint32_t total = r_it.old_count + r_it.spawn;
		uint32_t write = 0;
		uint32_t light_write = 0;
		std::array<float, kChunk> dt;
		std::array<uint8_t, kChunk> dead;
		std::array<float, kChunk> tn;
		OutputValues ov;
		for (uint32_t c0 = 0; c0 < total; c0 += uint32_t(kChunk)) {
			const uint32_t n = std::min<uint32_t>(uint32_t(kChunk), total - c0);
			for (uint32_t i = 0; i < n; ++i) {
				const uint32_t idx = c0 + i;
				if (idx < r_it.old_count) {
					dt[i] = r_it.dt_e;
				} else {
					const uint32_t k = idx - r_it.old_count;
					float f = 0;
					if (r_it.rate_dt > 0) {
						f = clampf((float(k) + 1.0f - r_it.a0) / r_it.rate_dt, 0.0f, 1.0f);
					}
					dt[i] = (1.0f - f) * r_it.dt_e;
				}
				dead[i] = 0;
			}
			update_chunk(e, inst, r_it, dst, base + c0, n, dt.data(), dead.data(), tn.data(), local);

			output_values(e, inst, r_it, dst, base + c0, n, tn.data(), ov);
			for (uint32_t i = 0; i < n; ++i) {
				const uint32_t from = base + c0 + i;
				if (dead[i] != 0) {
					continue;
				}
				const uint32_t to = base + write;
				if (to != from) {
					dst.move_one(from, to);
				}
				if (!emit(e, inst, r_it, dst, to, i, tn[i], ov, local, light_write)) {
					++r_it.nonfinite;
					continue;
				}
				++write;
			}
		}
		r_it.alive = write;
		r_it.lights = light_write;
	}

	void spawn_one(const EmitterDef &p_e, const EmitterInstance &p_inst, const Item &p_it, uint32_t p_k, Soa &r_s, uint32_t p_at, bool p_local) {
		const uint32_t serial = p_it.first_serial + p_k;
		const uint64_t seed = p_inst.seed;
		const float te = p_it.te_spawn;
		const auto value = [&](ProcessorType p_type) {
			return slot_value(p_e, p_type, input_of(p_e.slot(p_type).input, 0.0f, te, 0.0f), seed, serial);
		};
		const ProcessorParams &pp = p_e.params;
		Vec3 pos{};
		if (p_e.has(ProcessorType::SpawnPosition)) {
			const Vec4 v = value(ProcessorType::SpawnPosition);
			pos = { v.x, v.y, v.z };
		}
		Vec3 dir{};
		if (p_e.has(ProcessorType::SpawnDirection)) {
			const Vec4 v = value(ProcessorType::SpawnDirection);
			dir = normalize_or_zero(Vec3{ v.x, v.y, v.z });
			const float k = pp.direction_from_emitter_origin;
			if (k != 0) {
				const Vec3 radial = normalize_or_zero(pos);
				dir = normalize_or_zero(Vec3{ dir.x + (radial.x - dir.x) * k, dir.y + (radial.y - dir.y) * k, dir.z + (radial.z - dir.z) * k });
			}
		}
		Vec3 vel{};
		if (p_e.has(ProcessorType::SpawnSpeed)) {
			vel = mul(dir, pp.speed * value(ProcessorType::SpawnSpeed).x);
		}
		Vec3 size{ 1, 1, 1 };
		if (p_e.has(ProcessorType::SpawnSize)) {
			const Vec4 v = value(ProcessorType::SpawnSize);
			size = { pp.size * v.x, pp.size * v.y, pp.size * v.z };
		}
		const float rotation = p_e.has(ProcessorType::SpawnRotation) ? pp.rotation * value(ProcessorType::SpawnRotation).x * kPi : 0.0f;
		const float rotation_speed = p_e.has(ProcessorType::SpawnRotationSpeed) ? pp.rotation_speed * value(ProcessorType::SpawnRotationSpeed).x * kPi : 0.0f;
		Vec3 axis{};
		if (p_e.has(ProcessorType::SpawnOrientation)) {
			const Vec4 v = value(ProcessorType::SpawnOrientation);
			axis = { v.x, v.y, v.z };
		}
		float frame = 0;
		if (p_e.has(ProcessorType::SpawnAnimationFrame)) {
			frame = p_e.slot(ProcessorType::SpawnAnimationFrame).evaluator != kNoEvaluator ? value(ProcessorType::SpawnAnimationFrame).x : float(pp.animation_frame);
		}
		float frame_rate = 0;
		if (p_e.has(ProcessorType::SpawnAnimation)) {
			frame_rate = pp.animation_speed * (p_e.slot(ProcessorType::SpawnAnimation).evaluator != kNoEvaluator ? value(ProcessorType::SpawnAnimation).x : 1.0f);
		}
		const float life = p_e.has(ProcessorType::UpdateAge) ? pp.particle_lifetime * value(ProcessorType::UpdateAge).x : 0.0f;
		float light = 1.0f;
		if ((p_e.tmpl.flags & kEmitterActAsPointLight) != 0) {
			light = fx_uniform(p_e.tmpl.light.random_min, p_e.tmpl.light.random_max, draw(seed, serial, kSlotLight, 0));
		}
		Vec3 inherit{};
		if (p_e.has(ProcessorType::SpawnDirection) && pp.inherit_speed_and_direction) {
			inherit = p_inst.source_velocity;
		}
		if (p_local) {
			vel = add(vel, unrotate(p_inst.transform, inherit));
		} else {
			pos = apply(p_inst.transform, pos);
			vel = add(rotate(p_inst.transform, vel), inherit);
		}
		r_s.f[kPx][p_at] = pos.x;
		r_s.f[kPy][p_at] = pos.y;
		r_s.f[kPz][p_at] = pos.z;
		r_s.f[kVx][p_at] = vel.x;
		r_s.f[kVy][p_at] = vel.y;
		r_s.f[kVz][p_at] = vel.z;
		r_s.f[kSx][p_at] = size.x;
		r_s.f[kSy][p_at] = size.y;
		r_s.f[kSz][p_at] = size.z;
		r_s.f[kRot][p_at] = rotation;
		r_s.f[kRspd][p_at] = rotation_speed;
		r_s.f[kFrame][p_at] = frame;
		r_s.f[kFrate][p_at] = frame_rate;
		r_s.f[kAge][p_at] = 0.0f;
		r_s.f[kLife][p_at] = life;
		r_s.f[kAx][p_at] = axis.x;
		r_s.f[kAy][p_at] = axis.y;
		r_s.f[kAz][p_at] = axis.z;
		r_s.f[kLight][p_at] = light;
		r_s.serial[p_at] = serial;
	}

	struct Values {
		bool constant = true;
		Vec4 c;
		std::array<float, kChunk> x, y, z;
	};

	static bool draws(EvaluatorType p_type) {
		return p_type == EvaluatorType::Random || p_type == EvaluatorType::RandomXyz || p_type == EvaluatorType::Box || p_type == EvaluatorType::Sphere ||
				p_type == EvaluatorType::RotateVector;
	}

	void values(const EmitterDef &p_e, ProcessorType p_type, const Item &p_it, const EmitterInstance &p_inst, const Soa &p_s, uint32_t p_at, uint32_t p_n,
			const float *p_t, bool p_xyz, Values &r_v) const {
		const ProcessorSlot &slot = p_e.slot(p_type);
		if (slot.evaluator == kNoEvaluator) {
			r_v.constant = true;
			r_v.c = slot.default_values;
			return;
		}
		const EvaluatorDef &ev = p_e.evaluators[size_t(slot.evaluator)];
		const bool per_particle_input = slot.input == EvaluatorInput::NormTime || slot.input == EvaluatorInput::Velocity;
		if (!per_particle_input && !draws(ev.type)) {
			r_v.constant = true;
			r_v.c = eval_one(p_e, ev, uint32_t(p_type), input_of(slot.input, 0, p_it.te, 0), p_inst.seed, 0, slot.default_values);
			return;
		}
		r_v.constant = false;
		if (slot.input == EvaluatorInput::NormTime && ev.type == EvaluatorType::Polynomial) {
			const PolynomialDef &pd = ev.polynomial;
			const Vec4 c = pd.coefficients;
			for (uint32_t i = 0; i < p_n; ++i) {
				const float tt = p_t[i];
				const float f = ((c.x * tt + c.y) * tt + c.z) * tt + c.w;
				r_v.x[i] = clampf(f, pd.min_clamp, pd.max_clamp) * pd.scale;
			}
			if (p_xyz) {
				std::memcpy(r_v.y.data(), r_v.x.data(), p_n * sizeof(float));
				std::memcpy(r_v.z.data(), r_v.x.data(), p_n * sizeof(float));
			}
			return;
		}
		if (slot.input == EvaluatorInput::NormTime && ev.type == EvaluatorType::PolynomialColorInterp) {
			const ColorInterpDef &ci = ev.color_interp;
			const Vec4 c = ci.coefficients;
			for (uint32_t i = 0; i < p_n; ++i) {
				const float tt = p_t[i];
				const float f = clampf(((c.x * tt + c.y) * tt + c.z) * tt + c.w, 0.0f, 1.0f);
				r_v.x[i] = ci.color1.x + (ci.color0.x - ci.color1.x) * f;
				r_v.y[i] = ci.color1.y + (ci.color0.y - ci.color1.y) * f;
				r_v.z[i] = ci.color1.z + (ci.color0.z - ci.color1.z) * f;
			}
			return;
		}
		for (uint32_t i = 0; i < p_n; ++i) {
			const uint32_t k = p_at + i;
			float speed = 0;
			if (slot.input == EvaluatorInput::Velocity) {
				speed = std::sqrt(p_s.f[kVx][k] * p_s.f[kVx][k] + p_s.f[kVy][k] * p_s.f[kVy][k] + p_s.f[kVz][k] * p_s.f[kVz][k]);
			}
			const Vec4 v = eval_one(p_e, ev, uint32_t(p_type), input_of(slot.input, p_t[i], p_it.te, speed), p_inst.seed, p_s.serial[k], slot.default_values);
			r_v.x[i] = v.x;
			r_v.y[i] = v.y;
			r_v.z[i] = v.z;
		}
	}

	void update_chunk(const EmitterDef &p_e, const EmitterInstance &p_inst, const Item &p_it, Soa &r_s, uint32_t p_at, uint32_t p_n, const float *p_dt,
			uint8_t *r_dead, float *r_t, bool p_local) {
		const ProcessorParams &pp = p_e.params;
		float *px = r_s.f[kPx].data() + p_at;
		float *py = r_s.f[kPy].data() + p_at;
		float *pz = r_s.f[kPz].data() + p_at;
		float *vx = r_s.f[kVx].data() + p_at;
		float *vy = r_s.f[kVy].data() + p_at;
		float *vz = r_s.f[kVz].data() + p_at;
		float *age = r_s.f[kAge].data() + p_at;
		const float *life = r_s.f[kLife].data() + p_at;
		float *t = r_t;
		Values v;

		const bool has_age = p_e.has(ProcessorType::UpdateAge);
		for (uint32_t i = 0; i < p_n; ++i) {
			const float before = age[i];
			age[i] = before + p_dt[i];
			if (life[i] > 0) {
				const float tt = age[i] / life[i];
				t[i] = tt < 1.0f ? tt : 1.0f;
				r_dead[i] = tt >= 1.0f ? 1 : 0;
			} else {
				t[i] = 0.0f;
				r_dead[i] = (has_age && before > 0) ? 1 : 0;
			}
		}
		if (p_it.kill_all || (!has_age && p_it.window_over)) {
			for (uint32_t i = 0; i < p_n; ++i) {
				r_dead[i] = 1;
			}
			return;
		}

		if (p_e.has(ProcessorType::AirResistance)) {
			values(p_e, ProcessorType::AirResistance, p_it, p_inst, r_s, p_at, p_n, t, false, v);
			const float f_full = v.constant ? std::exp(-(pp.drag * v.c.x) * p_it.dt_e) : 0.0f;
			for (uint32_t i = 0; i < p_n; ++i) {
				const float k = pp.drag * (v.constant ? v.c.x : v.x[i]);
				const float f = v.constant && p_dt[i] == p_it.dt_e ? f_full : std::exp(-k * p_dt[i]);
				vx[i] *= f;
				vy[i] *= f;
				vz[i] *= f;
			}
		}

		if (p_e.has(ProcessorType::Gravity)) {
			values(p_e, ProcessorType::Gravity, p_it, p_inst, r_s, p_at, p_n, t, false, v);
			const Vec3 down = p_local ? unrotate(p_inst.transform, Vec3{ 0, -1, 0 }) : Vec3{ 0, -1, 0 };
			for (uint32_t i = 0; i < p_n; ++i) {
				const float a = pp.gravity * (v.constant ? v.c.x : v.x[i]) * p_dt[i];
				vx[i] += down.x * a;
				vy[i] += down.y * a;
				vz[i] += down.z * a;
			}
		}

		if (p_e.has(ProcessorType::LocalForce)) {
			values(p_e, ProcessorType::LocalForce, p_it, p_inst, r_s, p_at, p_n, t, true, v);
			const Vec3 &f = pp.local_force;
			for (uint32_t i = 0; i < p_n; ++i) {
				const Vec3 lf = v.constant ? Vec3{ f.x * v.c.x, f.y * v.c.y, f.z * v.c.z } : Vec3{ f.x * v.x[i], f.y * v.y[i], f.z * v.z[i] };
				const Vec3 a = p_local ? lf : rotate(p_inst.transform, lf);
				vx[i] += a.x * p_dt[i];
				vy[i] += a.y * p_dt[i];
				vz[i] += a.z * p_dt[i];
			}
		}

		if (p_e.has(ProcessorType::Turbulance) && pp.turbulence != 0) {
			values(p_e, ProcessorType::Turbulance, p_it, p_inst, r_s, p_at, p_n, t, false, v);
			const uint32_t *serial = r_s.serial.data() + p_at;
			for (uint32_t i = 0; i < p_n; ++i) {
				const float a = age[i] * kTurbulenceKnotsPerSecond;
				const float kf = std::floor(std::fmax(0.0f, a));
				const uint32_t k = uint32_t(std::fmin(kf, 4.0e9f));
				const float frac = std::fmax(0.0f, a) - kf;
				Vec3 n0{}, n1{};
				float *n0p = &n0.x;
				float *n1p = &n1.x;
				for (uint32_t axis = 0; axis < 3; ++axis) {
					n0p[axis] = 2.0f * draw(p_inst.seed, serial[i], kSlotTurbulence, k * 3 + axis) - 1.0f;
					n1p[axis] = 2.0f * draw(p_inst.seed, serial[i], kSlotTurbulence, (k + 1) * 3 + axis) - 1.0f;
				}
				const Vec3 nv = normalize_or_zero(Vec3{ n0.x + (n1.x - n0.x) * frac, n0.y + (n1.y - n0.y) * frac, n0.z + (n1.z - n0.z) * frac });
				const float s = pp.turbulence * (v.constant ? v.c.x : v.x[i]) * p_dt[i];
				vx[i] += nv.x * s;
				vy[i] += nv.y * s;
				vz[i] += nv.z * s;
			}
		}

		if (p_e.has(ProcessorType::UpdateLinearVelocity)) {
			for (uint32_t i = 0; i < p_n; ++i) {
				px[i] += vx[i] * p_dt[i];
				py[i] += vy[i] * p_dt[i];
				pz[i] += vz[i] * p_dt[i];
			}
		}
		if (p_e.has(ProcessorType::UpdateRotation)) {
			values(p_e, ProcessorType::UpdateRotation, p_it, p_inst, r_s, p_at, p_n, t, false, v);
			float *rot = r_s.f[kRot].data() + p_at;
			const float *rspd = r_s.f[kRspd].data() + p_at;
			for (uint32_t i = 0; i < p_n; ++i) {
				rot[i] += rspd[i] * (v.constant ? v.c.x : v.x[i]) * p_dt[i];
			}
		}
		if (p_e.has(ProcessorType::UpdateTextureCoords)) {
			values(p_e, ProcessorType::UpdateTextureCoords, p_it, p_inst, r_s, p_at, p_n, t, false, v);
			float *frame = r_s.f[kFrame].data() + p_at;
			const float *frate = r_s.f[kFrate].data() + p_at;

			const float count = float(p_e.texture.frame_count);
			for (uint32_t i = 0; i < p_n; ++i) {
				float f = frame[i] + frate[i] * (v.constant ? v.c.x : v.x[i]) * p_dt[i];
				if (f >= count && f < 2.0f * count) {
					f -= count;
				} else if (f < 0.0f && f >= -count) {
					f += count;
				}
				frame[i] = f;
			}
		}
	}

	static float norm_time(float p_age, float p_life) {
		if (p_life > 0) {
			const float tt = p_age / p_life;
			return tt < 1.0f ? tt : 1.0f;
		}
		return 0.0f;
	}

	struct OutputValues {
		Values size, size_x, size_y, size_z, color, alpha, alpha_min, alpha_max, alpha_scale;
		bool has_size = false, has_size_x = false, has_size_y = false, has_size_z = false, has_color = false, color_eval = false, has_alpha = false,
			 has_alpha_min = false, has_alpha_max = false, has_alpha_scale = false;
	};

	void output_values(const EmitterDef &p_e, const EmitterInstance &p_inst, const Item &p_it, const Soa &p_s, uint32_t p_at, uint32_t p_n, const float *p_t,
			OutputValues &r_o) const {
		const auto one = [&](ProcessorType p_type, bool p_xyz, bool &r_has, Values &r_v) {
			r_has = p_e.has(p_type);
			if (r_has) {
				values(p_e, p_type, p_it, p_inst, p_s, p_at, p_n, p_t, p_xyz, r_v);
			}
		};
		one(ProcessorType::UpdateSize, true, r_o.has_size, r_o.size);
		one(ProcessorType::UpdateSizeX, false, r_o.has_size_x, r_o.size_x);
		one(ProcessorType::UpdateSizeY, false, r_o.has_size_y, r_o.size_y);
		one(ProcessorType::UpdateSizeZ, false, r_o.has_size_z, r_o.size_z);
		r_o.has_color = p_e.has(ProcessorType::UpdateColor);
		r_o.color_eval = r_o.has_color && p_e.slot(ProcessorType::UpdateColor).evaluator != kNoEvaluator;
		if (r_o.color_eval) {
			values(p_e, ProcessorType::UpdateColor, p_it, p_inst, p_s, p_at, p_n, p_t, true, r_o.color);
		}
		one(ProcessorType::UpdateTransparency, false, r_o.has_alpha, r_o.alpha);
		one(ProcessorType::UpdateAlphaLevelMin, false, r_o.has_alpha_min, r_o.alpha_min);
		one(ProcessorType::UpdateAlphaLevelMax, false, r_o.has_alpha_max, r_o.alpha_max);
		one(ProcessorType::UpdateAlphaLevelScale, false, r_o.has_alpha_scale, r_o.alpha_scale);
	}

	static float vx_of(const Values &p_v, uint32_t p_i) { return p_v.constant ? p_v.c.x : p_v.x[p_i]; }
	static Vec3 v3_of(const Values &p_v, uint32_t p_i) { return p_v.constant ? Vec3{ p_v.c.x, p_v.c.y, p_v.c.z } : Vec3{ p_v.x[p_i], p_v.y[p_i], p_v.z[p_i] }; }

	bool emit(const EmitterDef &p_e, const EmitterInstance &p_inst, const Item &p_it, const Soa &p_s, uint32_t p_at, uint32_t p_i, float p_t,
			const OutputValues &p_o, bool p_local, uint32_t &r_light) {
		const float t = p_t;
		const uint32_t serial = p_s.serial[p_at];
		const Vec3 vel{ p_s.f[kVx][p_at], p_s.f[kVy][p_at], p_s.f[kVz][p_at] };
		const ProcessorParams &pp = p_e.params;
		ParticleVertex out;
		Vec3 pos{ p_s.f[kPx][p_at], p_s.f[kPy][p_at], p_s.f[kPz][p_at] };
		Vec3 wvel = vel;
		if (p_local) {
			pos = apply(p_inst.transform, pos);
			wvel = rotate(p_inst.transform, vel);
		}
		out.position = pos;
		out.rotation = p_s.f[kRot][p_at];

		Vec3 size{ p_s.f[kSx][p_at], p_s.f[kSy][p_at], p_s.f[kSz][p_at] };
		if (p_o.has_size) {
			const Vec3 v = v3_of(p_o.size, p_i);
			size = { size.x * v.x, size.y * v.y, size.z * v.z };
		}
		if (p_o.has_size_x) {
			size.x *= vx_of(p_o.size_x, p_i);
		}
		if (p_o.has_size_y) {
			size.y *= vx_of(p_o.size_y, p_i);
		}
		if (p_o.has_size_z) {
			size.z *= vx_of(p_o.size_z, p_i);
		}
		out.size = size;

		float frame = p_s.f[kFrame][p_at];
		const float count = float(p_e.texture.frame_count);
		if (count > 0 && (frame < 0 || frame >= count)) {
			frame = std::fmod(frame, count);
			if (frame < 0) {
				frame += count;
			}
			if (frame >= count) {
				frame = 0;
			}
		}
		out.frame = frame;
		Vec3 rgb{ 1, 1, 1 };
		if (p_o.has_color) {
			rgb = pp.color;
			if (p_o.color_eval) {
				const Vec3 v = v3_of(p_o.color, p_i);
				rgb = { rgb.x * v.x, rgb.y * v.y, rgb.z * v.z };
			}
		}
		const float alpha = p_o.has_alpha ? vx_of(p_o.alpha, p_i) : 1.0f;
		out.color = { rgb.x, rgb.y, rgb.z, alpha };
		if (p_e.tmpl.emittable == EmittableType::ParticleMesh) {
			out.axis = { p_s.f[kAx][p_at], p_s.f[kAy][p_at], p_s.f[kAz][p_at] };
		} else if (p_e.tmpl.emittable == EmittableType::DirectionAlignedQuad) {
			out.axis = normalize_or_zero(wvel);
		}
		out.age = t;
		out.alpha_min = p_o.has_alpha_min ? pp.alpha_min_level * vx_of(p_o.alpha_min, p_i) : 0.0f;
		out.alpha_max = p_o.has_alpha_max ? pp.alpha_max_level * vx_of(p_o.alpha_max, p_i) : 1.0f;
		out.alpha_scale = p_o.has_alpha_scale ? vx_of(p_o.alpha_scale, p_i) : 1.0f;
		out.serial = serial;

		const float p0 = (out.position.x + out.position.y) + (out.position.z + vel.x);
		const float p1 = (vel.y + vel.z) + (out.size.x + out.size.y);
		const float p2 = (out.size.z + out.color.x) + (out.color.y + out.color.z);
		const float p3 = (out.color.w + out.frame) + (out.rotation + out.axis.x);
		const float p4 = (out.axis.y + out.axis.z) + (out.alpha_min + out.alpha_max);
		const float probe = (((p0 + p1) + (p2 + p3)) + (p4 + out.alpha_scale)) * 0.0f;
		if (probe != 0.0f) {
			return false;
		}
		ParticleLight light;
		if (p_it.light_first != kNone) {
			const PointLightDef &l = p_e.tmpl.light;
			const float intensity = evaluate_polynomial(l.intensity, t) * p_s.f[kLight][p_at];
			light.position = add(pos, rotate(p_inst.transform, l.pivot));
			light.radius = l.radius;
			light.color = mul(l.color, intensity);
			light.intensity = intensity;
			if (!finite3(light.position.x, light.position.y, light.position.z) || !finite3(light.color.x, light.color.y, light.color.z) ||
					!std::isfinite(light.intensity)) {
				return false;
			}
		}

		vertices[p_at] = out;
		if (p_it.light_first != kNone) {
			lights[p_it.light_first + r_light] = light;
			++r_light;
		}
		return true;
	}

	void end_step() {
		if (substep >= substeps) {
			return;
		}
		const int next = 1 - cur;
		Soa &s = buf[next];
		uint32_t live = 0;
		for (Item &it : items) {
			EmitterInstance &inst = emitters[it.instance];
			inst.state_first = it.first;
			inst.state_count = it.alive;
			live += it.alive;
			stats.spawned += it.spawn;
			stats.died += uint64_t(it.old_count) + it.spawn - it.alive;
			stats.nonfinite_kills += it.nonfinite;
		}
		live_particles = live;
		cur = next;

		const size_t planned = items.size();
		for (size_t i = 0; i < planned; ++i) {
			const Item &it = items[i];
			const uint32_t pid = it.instance;
			if (it.spawn == 0) {
				continue;
			}
			const EffectDef &d = *effects[emitters[pid].effect].def;
			const EmitterDef &pe = d.emitters[emitters[pid].emitter];
			if (pe.children.empty()) {
				continue;
			}
			const uint32_t first = emitters[pid].state_first;
			const uint32_t count = emitters[pid].state_count;
			for (uint32_t k = 0; k < count; ++k) {
				const uint32_t serial = s.serial[first + k];
				if (serial < it.first_serial) {
					continue;
				}
				for (uint32_t c = 0; c < pe.children.size(); ++c) {
					EmitterInstance &p = emitters[pid];
					if (p.live_children[c] >= pe.tmpl.max_count) {
						++stats.emitter_starts_dropped;
						continue;
					}
					const uint32_t ce = pe.children[c];
					const uint32_t id = new_emitter(p.effect, ce, p.component, fx_child_seed(p.seed, ce, serial));
					if (id == kNone) {
						continue;
					}
					EmitterInstance &ci = emitters[id];
					EmitterInstance &pp = emitters[pid];
					ci.parent = pid;
					ci.parent_generation = pp.generation;
					ci.parent_serial = serial;
					ci.parent_child_slot = c;
					++pp.live_children[c];
				}
			}
		}

		for (size_t i = 0; i < planned; ++i) {
			const uint32_t id = items[i].instance;
			EmitterInstance &inst = emitters[id];
			if (!inst.active) {
				continue;
			}
			const EmitterDef &e = effects[inst.effect].def->emitters[inst.emitter];
			const bool repeat = (e.tmpl.flags & kEmitterRepeatParticleSpawning) != 0;
			const bool window_over = inst.stopped || (e.tmpl.lifetime > 0 && !repeat && inst.e_age >= e.tmpl.lifetime);
			if (inst.state_count == 0 && (window_over || inst.kill)) {
				retire_emitter(id);
			}
		}

		outputs.clear();
		uint32_t vout = 0;
		uint32_t lout = 0;
		for (size_t i = 0; i < planned; ++i) {
			const Item &it = items[i];
			const EmitterInstance &inst = emitters[it.instance];
			if (!inst.active) {
				continue;
			}
			EmitterOutput o;
			const EffectSlot &fx = effects[inst.effect];
			o.effect = EffectHandle{ inst.effect, fx.generation };
			o.def = fx.def.get();
			o.emitter = inst.emitter;
			o.component = inst.component;
			o.first_person = fx.start.first_person;
			o.first_particle = vout;
			o.particle_count = it.alive;
			if (it.alive > 0 && it.first != vout) {
				std::memmove(vertices.data() + vout, vertices.data() + it.first, size_t(it.alive) * sizeof(ParticleVertex));
			}
			vout += it.alive;
			o.first_light = lout;
			o.light_count = it.light_first == kNone ? 0 : it.lights;
			if (o.light_count > 0 && it.light_first != lout) {
				std::memmove(lights.data() + lout, lights.data() + it.light_first, size_t(o.light_count) * sizeof(ParticleLight));
			}
			lout += o.light_count;
			o.emitter_transform = inst.transform;
			outputs.push_back(o);
		}
		particle_out = vout;
		light_out = lout;
		sweep();
		++substep;
		++stats.substeps;
		stats.live_effects = uint32_t(effect_list.size());
		stats.live_emitters = uint32_t(emitter_list.size());
		stats.live_particles = live_particles;
		stats.live_lights = lout;
	}
};

FxWorld::FxWorld(const FxWorldConfig &p_config) :
		impl_(std::make_unique<Impl>(p_config)) {}

FxWorld::~FxWorld() = default;

core::Result<EffectHandle> FxWorld::start(std::shared_ptr<const EffectDef> p_def, const EffectStart &p_start) {
	Impl &w = *impl_;
	if (!p_def) {
		++w.stats.refused_starts;
		return core::make_error("start: no def");
	}
	if (p_def->max_particles > w.cfg.max_particles || p_def->components.size() > kMaxEffectComponents) {
		++w.stats.refused_starts;
		return core::make_error("start: " + p_def->name + " needs " + std::to_string(p_def->max_particles) + " particles, the world holds " +
				std::to_string(w.cfg.max_particles));
	}

	uint32_t live = 0;
	uint32_t oldest = kNone;
	for (const uint32_t slot : w.effect_list) {
		const Impl::EffectSlot &s = w.effects[slot];
		if (s.active && !s.killed && s.def == p_def) {
			++live;
			if (oldest == kNone || s.sequence < w.effects[oldest].sequence) {
				oldest = slot;
			}
		}
	}
	if (live >= p_def->max_instance_count) {
		if (!p_def->reset_instance_when_started || oldest == kNone) {
			++w.stats.refused_starts;
			return core::make_error("start: " + p_def->name + ": instance pool full");
		}
		w.effects[oldest].killed = true;
		++w.stats.stolen_instances;
	}
	if (w.free_effects.empty()) {
		++w.stats.refused_starts;
		return core::make_error("start: world holds " + std::to_string(w.cfg.max_effects) + " effects");
	}
	const uint32_t slot = w.free_effects.back();
	w.free_effects.pop_back();
	Impl::EffectSlot &s = w.effects[slot];
	const uint32_t gen = s.generation + 1 == 0 ? 1 : s.generation + 1;
	s = Impl::EffectSlot{};
	s.generation = gen;
	s.active = true;
	s.def = std::move(p_def);
	s.start = p_start;
	s.start.start_age = std::isfinite(p_start.start_age) ? clampf(p_start.start_age, 0.0f, kMaxStartAge) : 0.0f;
	s.base = compose(s.start.transform, s.def->transform);
	s.age = s.start.start_age;
	s.sequence = ++w.sequence;
	for (uint32_t c = 0; c < s.def->components.size(); ++c) {
		if (fx_component_selected(*s.def, c, s.start.seed, s.start.tier)) {
			Impl::set_bit(s.selected, c);
		}
	}
	w.effect_list.push_back(slot);
	return EffectHandle{ slot, gen };
}

void FxWorld::stop(EffectHandle p_effect, StopMode p_mode) {
	Impl &w = *impl_;
	if (!alive(p_effect)) {
		return;
	}
	Impl::EffectSlot &s = w.effects[p_effect.slot];
	if (p_mode == StopMode::Kill) {
		s.killed = true;
	} else {
		s.stopping = true;
	}
}

void FxWorld::clear() {
	Impl &w = *impl_;
	for (Impl::EffectSlot &s : w.effects) {
		s.active = false;
		s.killed = false;
		s.def.reset();
	}
	for (Impl::EmitterInstance &e : w.emitters) {
		e.active = false;
	}
	w.effect_list.clear();
	w.emitter_list.clear();
	w.items.clear();
	w.outputs.clear();
	w.events.clear();
	w.particle_out = 0;
	w.light_out = 0;
	w.live_particles = 0;
	w.substeps = 0;
	w.substep = 0;
	w.sequence = 0;
	w.stats = FxStats{};
	w.reset_free_lists();
}

void FxWorld::set_transform(EffectHandle p_effect, const Transform &p_transform, const Vec3 &p_velocity) {
	if (!alive(p_effect)) {
		return;
	}
	Impl::EffectSlot &s = impl_->effects[p_effect.slot];
	s.start.transform = p_transform;
	s.start.velocity = p_velocity;
	s.base = compose(p_transform, s.def->transform);
}

void FxWorld::set_params(EffectHandle p_effect, const EmitterParams &p_params) {
	if (!alive(p_effect)) {
		return;
	}
	impl_->effects[p_effect.slot].start.params = p_params;
}

bool FxWorld::alive(EffectHandle p_effect) const {
	const Impl &w = *impl_;
	if (!p_effect.valid() || p_effect.slot >= w.effects.size()) {
		return false;
	}
	const Impl::EffectSlot &s = w.effects[p_effect.slot];
	return s.active && !s.killed && s.generation == p_effect.generation;
}

uint32_t FxWorld::begin_frame(float p_dt, const FxView &p_view) {
	(void)p_view;
	Impl &w = *impl_;
	w.events.clear();
	w.substep = 0;
	w.substeps = 0;
	if (!std::isfinite(p_dt) || p_dt <= 0) {
		return 0;
	}
	if (p_dt > w.cfg.max_dt) {
		p_dt = w.cfg.max_dt;
		++w.stats.dt_clamped;
	}
	const float k = std::ceil(p_dt / w.cfg.max_substep);
	w.substeps = std::max(1u, uint32_t(std::fmin(k, 1.0e6f)));
	w.sub_dt = p_dt / float(w.substeps);
	++w.stats.steps;
	return w.substeps;
}

size_t FxWorld::begin_step() {
	return impl_->begin_step();
}

void FxWorld::run_items(size_t p_first, size_t p_count) {
	Impl &w = *impl_;
	const size_t end = std::min(w.items.size(), p_first + std::min(p_count, w.items.size()));
	for (size_t i = p_first; i < end; ++i) {
		w.run_item(w.items[i]);
	}
}

void FxWorld::end_step() {
	impl_->end_step();
}

void FxWorld::step(float p_dt, const FxView &p_view, unsigned p_lanes) {
	const uint32_t k = begin_frame(p_dt, p_view);
	for (uint32_t s = 0; s < k; ++s) {
		const size_t n = begin_step();
		if (p_lanes <= 1 || n <= 1) {
			run_items(0, n);
		} else {
			core::parallel_for(n, [&](size_t i) { run_items(i, 1); }, p_lanes);
		}
		end_step();
	}
}

std::span<const EmitterOutput> FxWorld::outputs() const {
	return impl_->outputs;
}
std::span<const ParticleVertex> FxWorld::particles() const {
	return std::span<const ParticleVertex>(impl_->vertices.data(), impl_->particle_out);
}
std::span<const ParticleLight> FxWorld::lights() const {
	return std::span<const ParticleLight>(impl_->lights.data(), impl_->light_out);
}
std::span<const FxEvent> FxWorld::events() const {
	return impl_->events;
}
const FxStats &FxWorld::stats() const {
	return impl_->stats;
}
const FxWorldConfig &FxWorld::config() const {
	return impl_->cfg;
}

}
