#pragma once

#include "bf3/core/result.h"
#include "bf3/fx/effect_def.h"
#include "bf3/fx/fx_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::fx {

struct ParticleVertex {
	Vec3 position;
	float rotation = 0;
	Vec3 size;
	float frame = 0;
	Vec4 color;
	Vec3 axis;
	float age = 0;
	float alpha_min = 0;
	float alpha_max = 1;
	float alpha_scale = 1;
	uint32_t serial = 0;
};
static_assert(sizeof(ParticleVertex) == 80);

struct ParticleLight {
	Vec3 position;
	float radius = 0;
	Vec3 color;
	float intensity = 0;
};
static_assert(sizeof(ParticleLight) == 32);

struct EffectHandle {
	uint32_t slot = 0;
	uint32_t generation = 0;
	bool valid() const { return generation != 0; }
	bool operator==(const EffectHandle &) const = default;
};

struct EmitterParams {
	float distance = 0;
	Vec3 vec;
	float vec_average = 0;
	float param1 = 0, param2 = 0, param3 = 0;
};

struct EffectStart {
	Transform transform;
	Vec3 velocity;
	uint64_t seed = 0;
	uint8_t tier = kTierHigh;
	bool first_person = false;

	float start_age = 0;
	EmitterParams params;
};

struct FxView {
	Vec3 camera_position;
	Vec3 camera_forward{ 0, 0, 1 };
};

enum class StopMode : uint8_t {
	Stop,
	Kill,
};

struct FxWorldConfig {
	uint32_t max_effects = 4096;
	uint32_t max_emitter_instances = 16384;
	uint32_t max_particles = 1u << 16;
	float max_substep = 0.25f;
	float max_dt = 2.0f;
	uint8_t tier = kTierHigh;
};

struct FxStats {
	uint32_t live_effects = 0;
	uint32_t live_emitters = 0;
	uint32_t live_particles = 0;
	uint32_t live_lights = 0;
	uint64_t steps = 0;
	uint64_t substeps = 0;
	uint64_t dt_clamped = 0;
	uint64_t spawned = 0;
	uint64_t died = 0;
	uint64_t spawns_capped = 0;
	uint64_t spawns_dropped = 0;
	uint64_t emitter_starts_dropped = 0;
	uint64_t nonfinite_kills = 0;
	uint64_t stolen_instances = 0;
	uint64_t refused_starts = 0;
	uint64_t events_dropped = 0;
};

struct EmitterOutput {
	EffectHandle effect;
	const EffectDef *def = nullptr;
	uint32_t emitter = 0;
	uint32_t component = 0;
	bool first_person = false;
	uint32_t first_particle = 0, particle_count = 0;
	uint32_t first_light = 0, light_count = 0;
	Transform emitter_transform;
};

struct FxEvent {
	enum class Kind : uint8_t { SoundStart, SoundStop, VisualEnvironmentStart, VisualEnvironmentStop };
	Kind kind = Kind::SoundStart;
	EffectHandle effect;
	uint32_t component = 0;
	const EffectDef *def = nullptr;
	Transform transform;
};

inline bool fx_component_selected(const EffectDef &p_def, uint32_t p_component, uint64_t p_start_seed, uint8_t p_tier) {
	if (p_component >= p_def.components.size()) {
		return false;
	}
	const EffectComponentDef &c = p_def.components[p_component];
	if ((p_def.tiers & p_tier) == 0 || (c.tiers & p_tier) == 0) {
		return false;
	}
	return fx_unit(fx_key(p_start_seed, kKeyComponentDraw, p_component, 0)) < c.spawn_probability;
}

class FxWorld {
public:
	explicit FxWorld(const FxWorldConfig &p_config = {});
	~FxWorld();
	FxWorld(const FxWorld &) = delete;
	FxWorld &operator=(const FxWorld &) = delete;

	core::Result<EffectHandle> start(std::shared_ptr<const EffectDef> p_def, const EffectStart &p_start);
	void stop(EffectHandle p_effect, StopMode p_mode = StopMode::Stop);

	void clear();
	void set_transform(EffectHandle p_effect, const Transform &p_transform, const Vec3 &p_velocity);
	void set_params(EffectHandle p_effect, const EmitterParams &p_params);

	bool alive(EffectHandle p_effect) const;

	uint32_t begin_frame(float p_dt, const FxView &p_view);

	size_t begin_step();
	void run_items(size_t p_first, size_t p_count);
	void end_step();

	void step(float p_dt, const FxView &p_view, unsigned p_lanes = 1);

	std::span<const EmitterOutput> outputs() const;
	std::span<const ParticleVertex> particles() const;
	std::span<const ParticleLight> lights() const;
	std::span<const FxEvent> events() const;
	const FxStats &stats() const;
	const FxWorldConfig &config() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
