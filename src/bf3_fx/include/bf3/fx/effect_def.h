#pragma once

#include "bf3/fx/fx_types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::fx {

struct ColorInterpDef {
	Vec4 coefficients{ 0, 0, 0, 1 };
	Vec3 color0;
	Vec3 color1;
};
struct OperatorDef {
	PolynomialDef first;
	PolynomialDef second;
	PolynomialOperation operation = PolynomialOperation::Multiplication;
	float min_result = 0;
	float max_result = 1;
};
struct RandomDef {
	float min = 0;
	float max = 1;
};
struct RandomXyzDef {
	Vec3 min;
	Vec3 max;
};
struct BoxDef {
	Vec3 dimensions;
	Vec3 pivot;
};
struct SphereDef {
	float radius = 1;
	Vec3 scale{ 1, 1, 1 };
	Vec3 pivot;
};
struct RotateVectorDef {
	float angle_degrees = 0;
	bool input_affects_phi = false;
	bool rotate_within_plane = false;
};
struct CameraProximityDef {
	float forward_offset = 0;
	float inner_radius = 0;
	Vec3 inner_radius_direction{ 0, 1, 0 };
	Vec3 offset;
	Vec3 size;
};
struct SampleTextureDef {
	uint32_t first = 0;
	uint32_t count = 0;
	Vec3 intensity_min;
	Vec3 intensity_max;
	Vec2 dimensions;
	Vec2 origin;
};

struct EvaluatorDef {
	EvaluatorType type = EvaluatorType::Polynomial;
	EmitterParameter parameter = EmitterParameter::None;
	union {
		PolynomialDef polynomial;
		ColorInterpDef color_interp;
		OperatorDef op;
		RandomDef random;
		RandomXyzDef random_xyz;
		BoxDef box;
		SphereDef sphere;
		RotateVectorDef rotate_vector;
		CameraProximityDef camera_proximity;
		SampleTextureDef sample_texture;
	};
	EvaluatorDef() :
			polynomial() {}
};

inline constexpr int16_t kNoEvaluator = -1;

struct ProcessorSlot {
	bool present = false;
	EvaluatorInput input = EvaluatorInput::NormTime;
	Vec4 default_values{ 1, 1, 1, 1 };
	int16_t evaluator = kNoEvaluator;
};

struct ProcessorParams {

	float spawn_rate = 0;
	bool distribute_over_time = true;
	bool distribute_over_distance = false;
	float trail_segment_length = 0;

	float direction_from_emitter_origin = 0;
	bool inherit_speed_and_direction = false;

	float speed = 0;
	float size = 0;
	float rotation = 0;
	float rotation_speed = 0;

	int32_t animation_frame = 0;
	float animation_speed = 0;

	float particle_lifetime = 0;

	float proximity_forward_offset = 0;
	Vec3 proximity_size;

	float drag = 0;
	float gravity = 0;
	Vec3 local_force;
	float turbulence = 0;

	Vec2 size_pivot;

	Vec3 color{ 1, 1, 1 };

	float cull_threshold = 0;

	float alpha_min_level = 0;
	float alpha_max_level = 1;
	float alpha_exponent = 1;

	float texture_blend = 1;
};

struct TextureRef {
	std::string texture;
	std::string normal_map;
	uint16_t frame_count = 1;
	uint16_t frame_columns = 1;
	bool animated = false;
};

enum EmitterFlags : uint32_t {
	kEmitterLocalSpace = 1u << 0,
	kEmitterFollowSpawnSource = 1u << 1,
	kEmitterActAsPointLight = 1u << 2,
	kEmitterEmissive = 1u << 3,
	kEmitterOpaque = 1u << 4,
	kEmitterForceFullRes = 1u << 5,
	kEmitterForceNiceSorting = 1u << 6,
	kEmitterKillParticlesWithEmitter = 1u << 7,
	kEmitterRepeatParticleSpawning = 1u << 8,
	kEmitterTransparencySunShadow = 1u << 9,
	kEmitterExclusionVolumeCull = 1u << 10,

	kEmitterDerived = 1u << 16,

	kEmitterChild = 1u << 17,
	kEmitterHasParameterisedEvaluator = 1u << 18,
};

struct PointLightDef {
	float radius = 0;
	Vec3 color;
	PolynomialDef intensity;
	float random_min = 1;
	float random_max = 1;
	Vec3 pivot;
};

struct EmitterTemplateDef {
	EmittableType emittable = EmittableType::ScreenAlignedQuad;
	float lifetime = 0;
	uint32_t lifetime_frame_count = 0;
	uint32_t max_count = 1;
	float time_scale = 1;
	uint32_t flags = 0;
	std::string mesh;
	PointLightDef light;

	float max_spawn_distance = 0;
	float visible_after_distance = 0;
	float mesh_culling_distance = 0;
	float min_screen_area = 0;
	float particle_culling_factor = 0;
	float distance_scale_near = 1;
	float distance_scale_far = 1;
	float distance_scale_length = 0;
	float soft_particles_fade = 0;
	float light_multiplier = 1;
	float light_wrap_around = 0;
	float vertex_pixel_lighting_blend = 0;
	float global_local_normal_blend = 1;
	std::vector<float> z_occlusion_lookup;
};

struct EmitterDef {
	std::string name;
	EmitterTemplateDef tmpl;
	std::array<ProcessorSlot, kProcessorTypeCount> slots{};
	ProcessorParams params;
	std::vector<EvaluatorDef> evaluators;
	std::vector<Vec4> gradients;
	std::vector<int16_t> clip_lookup;
	TextureRef texture;
	std::vector<uint32_t> children;
	std::string base;

	const ProcessorSlot &slot(ProcessorType p_type) const { return slots[size_t(p_type)]; }
	bool has(ProcessorType p_type) const { return slots[size_t(p_type)].present; }
};

enum class ComponentKind : uint8_t {
	Emitter,
	Sound,
	VisualEnvironment,
};

struct EffectComponentDef {
	ComponentKind kind = ComponentKind::Emitter;
	Transform transform;
	float spawn_probability = 1;
	float start_delay = 0;
	float cull_distance = 0;
	uint8_t tiers = kTierAll;

	uint16_t max_instance_count = 1;
	bool reset_instance_when_started = true;
	uint32_t emitter = 0;
	std::string asset;
};

struct EffectDef {
	std::string name;

	Transform transform;
	float cull_distance = 0;
	float start_delay = 0;
	uint8_t tiers = kTierAll;
	uint16_t max_instance_count = 1;
	bool reset_instance_when_started = true;
	std::vector<EffectComponentDef> components;
	std::vector<EmitterDef> emitters;
	uint32_t max_particles = 0;
};

}
