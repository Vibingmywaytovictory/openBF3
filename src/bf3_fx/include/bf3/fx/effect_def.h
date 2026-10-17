#pragma once
// Compiled effect definitions (docs/design/m2a-vfx1.md §6; plan §3.14, D19): what the compiler (effect_compiler.h)
// makes of one EffectBlueprint and its EmitterDocuments, what the cache stores (effect_cache.h) and what the
// simulator (simulator.h) runs. Flat, owned, level-independent data: no pointer into a partition, no EBX type, no
// atlas rectangle (atlas rects are per level, emitter_atlas.h). Godot-free.
//
// Shape of an emitter: the template (EmitterTemplateData) plus one optional slot per processor type. A type appears
// at most once per reachable chain (census), so a slot table loses nothing; the simulator walks the present slots in
// the canonical ProcessorType order (fx_types.h). Each slot holds its EvaluatorInput, DefaultValues and at most one
// evaluator (Pre). The value a processor reads is
//     v = Pre ? evaluate(Pre, input) : DefaultValues           (a Vec4; scalar evaluators broadcast)
// and the per-processor formula combines v with the processor's own parameter (the table in docs §6.4).

#include "bf3/fx/fx_types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::fx {

// ---- evaluators (Pre) --------------------------------------------------------------------------------------

struct ColorInterpDef { // PolynomialColorInterpData
	Vec4 coefficients{ 0, 0, 0, 1 };
	Vec3 color0; // reached at f = 1
	Vec3 color1; // reached at f = 0
};
struct OperatorDef { // PolynomialOperatorData: clamp(op(first(t), second(t)), MinClampResult, MaxClampResult)
	PolynomialDef first;
	PolynomialDef second;
	PolynomialOperation operation = PolynomialOperation::Multiplication;
	float min_result = 0;
	float max_result = 1;
};
struct RandomDef { // RandomEvaluatorData: lerp(Min, Max, u), u drawn once per particle and slot
	float min = 0;
	float max = 1;
};
struct RandomXyzDef { // RandomXYZEvaluatorData: per component
	Vec3 min;
	Vec3 max;
};
struct BoxDef { // BoxEvaluatorData: uniform in Pivot +- Dimensions / 2 [inferred: full extents]
	Vec3 dimensions;
	Vec3 pivot;
};
struct SphereDef { // SphereEvaluatorData: uniform in a ball of Radius, scaled by Scale, moved by Pivot [inferred]
	float radius = 1;
	Vec3 scale{ 1, 1, 1 };
	Vec3 pivot;
};
struct RotateVectorDef { // RotateVectorData: a direction inside a cone of Angle degrees around DefaultValues.xyz
	float angle_degrees = 0;
	bool input_affects_phi = false;
	bool rotate_within_plane = false; // true: the azimuth is fixed, the cone becomes a fan [inferred]
};
struct CameraProximityDef { // CameraProximityEvaluatorData: a spawn box around the camera (weather)
	float forward_offset = 0;
	float inner_radius = 0;
	Vec3 inner_radius_direction{ 0, 1, 0 };
	Vec3 offset;
	Vec3 size;
};
struct SampleTextureDef { // SampleTextureData: a 1-D gradient (rows of EmitterDef::gradients) sampled at t
	uint32_t first = 0; // index into EmitterDef::gradients
	uint32_t count = 0;
	Vec3 intensity_min;
	Vec3 intensity_max;
	Vec2 dimensions; // TextureDimensions
	Vec2 origin; // TextureOriginU, TextureOriginV
};

struct EvaluatorDef {
	EvaluatorType type = EvaluatorType::Polynomial;
	EmitterParameter parameter = EmitterParameter::None; // carried; VFX1 evaluates as None (docs §6.6)
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

// ---- processors --------------------------------------------------------------------------------------------

inline constexpr int16_t kNoEvaluator = -1;

struct ProcessorSlot {
	bool present = false; // the type is in the reachable chain and Enable is true
	EvaluatorInput input = EvaluatorInput::NormTime;
	Vec4 default_values{ 1, 1, 1, 1 }; // ProcessorData.DefaultValues: v when there is no Pre
	int16_t evaluator = kNoEvaluator; // index into EmitterDef::evaluators
};

// The processors' own fields (names as in EBX). Only the fields of present slots are meaningful.
struct ProcessorParams {
	// SpawnRateData
	float spawn_rate = 0; // particles per second (with MaxCount an independent cap)
	bool distribute_over_time = true; // false: the window's spawns all at its start
	bool distribute_over_distance = false; // spawns per metre of emitter travel [inferred]
	float trail_segment_length = 0;
	// SpawnDirectionData
	float direction_from_emitter_origin = 0;
	bool inherit_speed_and_direction = false; // InheritSpeedAndDirectionFromEmitter
	// SpawnSpeedData, SpawnSizeData, SpawnRotationData, SpawnRotationSpeedData
	float speed = 0;
	float size = 0;
	float rotation = 0;
	float rotation_speed = 0;
	// SpawnAnimationFrameData, SpawnAnimationData
	int32_t animation_frame = 0;
	float animation_speed = 0; // frames per second
	// UpdateAgeData
	// Seconds; 0 when the slot is absent: then particles live until their emitter instance ends. A particle's life is
	// particle_lifetime * v.x at spawn; when that is <= 0 (no slot, or hostile data) NormTime reads t = 0 instead of
	// dividing by it, and a slot-present particle with life <= 0 dies at the end of its first step (docs §6.5).
	float particle_lifetime = 0;
	// UpdateCameraProximityData
	float proximity_forward_offset = 0;
	Vec3 proximity_size;
	// AirResistanceData, GravityData, LocalForceData, TurbulanceData
	float drag = 0; // DragFactor
	float gravity = 0; // m/s^2 along world Y
	Vec3 local_force; // m/s^2 in the emitter's frame
	float turbulence = 0; // Intensity
	// UpdateSizeData
	Vec2 size_pivot;
	// UpdateColorData
	Vec3 color{ 1, 1, 1 };
	// UpdateTransparencyData
	float cull_threshold = 0; // carried for the renderer (VFX2)
	// UpdateAlphaLevelMinData, UpdateAlphaLevelMaxData, UpdateAlphaLevelScaleData
	float alpha_min_level = 0;
	float alpha_max_level = 1;
	float alpha_exponent = 1;
	// UpdateTextureCoordsData
	float texture_blend = 1; // TextureBlendValue
};

// UpdateTextureCoordsData.TextureInfo1 (EmitterTextureInfo); names lowercased, '\' as '/'.
// AnimationFrameCount and AnimationFrameColumnCount are floats in EBX (16.0 / 4.0 on the 1p flash). The compiler
// accepts only finite integral values in [0, kMaxFlipbookFrames] with columns <= frames, and stores 0 as 1 (retail:
// one document, em_aamissilemuzzleflash_01_3p, has 0 / 0 with EnableAnimation false); anything else fails the item.
// Retail pairs: 1..49 frames, 1..8 columns, columns <= frames on all 2,479 instances [verified 2026-10-15].
struct TextureRef {
	std::string texture; // "fx/textures/muzzleflash/muzzleflash_01": an EmitterSystemAsset.TextureInfos name
	std::string normal_map; // "systems/emitter/defaultnormalmap"
	uint16_t frame_count = 1; // AnimationFrameCount, 1 ... kMaxFlipbookFrames
	uint16_t frame_columns = 1; // AnimationFrameColumnCount, 1 ... frame_count
	bool animated = false; // EnableAnimation
};

// ---- emitters ----------------------------------------------------------------------------------------------

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
	// derived by the compiler
	kEmitterDerived = 1u << 16, // a BaseEmitterData document: processors from the base (docs §6.2)
	// Started per particle of a parent emitter. Roles are per def: the compiler deduplicates emitters by (document
	// name, role), so a document that one effect uses both as a component's emitter and as a child gets two defs, one
	// with and one without this flag; the census counts such documents (docs §6.2).
	kEmitterChild = 1u << 17,
	kEmitterHasParameterisedEvaluator = 1u << 18, // an evaluator names an EmitterParameter other than None
};

// EmitterTemplateData.PointLight* (ActAsPointLight emitters): one light per live particle; intensity =
// clamp(cubic(t), PointLightMinClamp, PointLightMaxClamp) * U[PointLightRandomIntensityMin, Max] per particle.
struct PointLightDef {
	float radius = 0;
	Vec3 color; // HDR linear
	PolynomialDef intensity; // Coefficients PointLightIntensity, clamps PointLightMin/MaxClamp, scale 1
	float random_min = 1;
	float random_max = 1;
	Vec3 pivot;
};

struct EmitterTemplateDef {
	EmittableType emittable = EmittableType::ScreenAlignedQuad;
	float lifetime = 0; // spawn window in seconds; 0 = continuous until stopped
	uint32_t lifetime_frame_count = 0;
	uint32_t max_count = 1; // hard cap on live particles of one emitter instance (1 ... kMaxEmitterMaxCount)
	float time_scale = 1; // scales the emitter's dt (spawning and ageing alike)
	uint32_t flags = 0; // EmitterFlags
	std::string mesh; // Mesh partition (ParticleMesh), lowercase; empty for quads
	PointLightDef light;
	// Render-side fields carried for VFX2 (the simulator does not read them).
	float max_spawn_distance = 0;
	float visible_after_distance = 0;
	float mesh_culling_distance = 0;
	float min_screen_area = 0;
	float particle_culling_factor = 0;
	float distance_scale_near = 1;
	float distance_scale_far = 1;
	float distance_scale_length = 0;
	float soft_particles_fade = 0; // SoftParticlesFadeDistanceMultiplier
	float light_multiplier = 1;
	float light_wrap_around = 0;
	float vertex_pixel_lighting_blend = 0;
	float global_local_normal_blend = 1;
	std::vector<float> z_occlusion_lookup;
};

struct EmitterDef {
	std::string name; // the EmitterDocument partition, lowercase
	EmitterTemplateDef tmpl;
	std::array<ProcessorSlot, kProcessorTypeCount> slots{};
	ProcessorParams params;
	std::vector<EvaluatorDef> evaluators;
	std::vector<Vec4> gradients; // SampleTextureData rows
	std::vector<int16_t> clip_lookup; // UpdateClipScaleData.Lookup, carried for VFX2
	TextureRef texture;
	std::vector<uint32_t> children; // EmitterData: indices into EffectDef::emitters
	std::string base; // BaseEmitterData's EmitterAsset (provenance; its processors are already merged)

	const ProcessorSlot &slot(ProcessorType p_type) const { return slots[size_t(p_type)]; }
	bool has(ProcessorType p_type) const { return slots[size_t(p_type)].present; }
};

// ---- effects -----------------------------------------------------------------------------------------------

enum class ComponentKind : uint8_t {
	Emitter, // EmitterEntityData
	Sound, // SoundEffectEntityData -> SoundPatchAsset (the audio track's seam, docs §7.6)
	VisualEnvironment, // VisualEnvironmentEffectEntityData (VFX5)
};

struct EffectComponentDef {
	ComponentKind kind = ComponentKind::Emitter;
	Transform transform; // relative to the effect (EffectDef::transform), docs §6.5
	float spawn_probability = 1; // EmitterEntityData only; drawn once per effect start
	float start_delay = 0; // added to EffectDef::start_delay
	float cull_distance = 0;
	uint8_t tiers = kTierAll;
	// EmitterEntityData.MaxInstanceCount / ResetInstanceWhenStarted (retail: 1 on 4,349, 20 on 3, 50 on 1; reset
	// false on 8). Carried in the blob, not applied in VFX1: the effect-level pool is the only instance pool (docs
	// §6.7, open Q2); VFX3's instancer decides whether a component pool applies, and per owner or not.
	uint16_t max_instance_count = 1;
	bool reset_instance_when_started = true;
	uint32_t emitter = 0; // Emitter: index into EffectDef::emitters
	std::string asset; // Sound: the SoundPatchAsset partition; VisualEnvironment: the visual environment partition
};

struct EffectDef {
	std::string name; // the EffectBlueprint partition, lowercase (a wrapped lone emitter: the EmitterDocument's)
	// EffectEntityData.Transform (non-identity on 21 of the 1,101 retail effects). A point p in a component's frame
	// lands in the world at  EffectStart::transform( transform( EffectComponentDef::transform(p) ) )  (docs §6.5).
	Transform transform;
	float cull_distance = 0;
	float start_delay = 0;
	uint8_t tiers = kTierAll;
	uint16_t max_instance_count = 1; // EffectEntityData.MaxInstanceCount: the pool size (docs §6.7)
	bool reset_instance_when_started = true; // true on all 1,101: a start past the pool steals the oldest
	std::vector<EffectComponentDef> components; // blueprint order
	std::vector<EmitterDef> emitters; // components' emitters first, then children; deduplicated by (name, role)
	uint32_t max_particles = 0; // sum of every emitter's MaxCount, children x their parent's MaxCount
};

} // namespace bf3::fx
