#pragma once
// bf3_fx vocabulary (docs/design/m2a-vfx1.md §5, §6; plan §3.14, D19): the 32 processor and 10 evaluator types of
// BF3's emitter graphs, the enum values their fields carry, the structural bounds every compiled def obeys, the
// polynomial evaluator and the counter-based random numbers the simulator draws. Godot-free; nothing here reads the
// install.
//
// Names are the EBX type and enumerator names as the descriptors spell them ("SpawnRateData", "EfNormTime"); the
// tables in fx_types.cpp map them both ways and are the only place a name is compared (the compiler reports any name
// it does not find there as unknown, and the census gate counts those: 0 over @all-mp).
//
// Processor order. Every type appears at most once in an emitter's reachable chain (census over the 3,410
// EmitterDocuments of @all-mp, 2026-10-15), but the chain order is authoring order and varies (SpawnDirection after
// SpawnSpeed on 443 emitters, UpdateAge after SpawnRate on 56), so the simulator runs each stage in the enum order
// below, never in chain order. The enum order is that canonical order.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bf3::fx {

struct Vec2 {
	float x = 0, y = 0;
	bool operator==(const Vec2 &) const = default;
};
struct Vec3 {
	float x = 0, y = 0, z = 0;
	bool operator==(const Vec3 &) const = default;
};
struct Vec4 {
	float x = 0, y = 0, z = 0, w = 0;
	bool operator==(const Vec4 &) const = default;
};

// Frostbite LinearTransform rows (right, up, forward, trans), metres, Y up; a local point p lands at
// trans + p.x*right + p.y*up + p.z*forward (bf3::core::level::LevelTransform without the separate scale).
struct Transform {
	Vec3 right{ 1, 0, 0 };
	Vec3 up{ 0, 1, 0 };
	Vec3 forward{ 0, 0, 1 };
	Vec3 trans{ 0, 0, 0 };
	bool operator==(const Transform &) const = default;
};

// ---- processors --------------------------------------------------------------------------------------------

enum class ProcessorStage : uint8_t {
	Structural, // resolved by the compiler: BaseEmitterData (derived emitter), EmitterData (child emitters)
	Emitter, // once per emitter instance and step: SpawnRateData
	Spawn, // once per new particle
	Update, // every live particle, every step
};

// The 32 ProcessorData types. Canonical order inside each stage (docs §6.3): forces accumulate after drag
// (AirResistance before Gravity on 2,243 of the 2,345 emitters that have both), and UpdateLinearVelocity integrates
// after every force (last of the forces on all 3,311 emitters that have it).
enum class ProcessorType : uint8_t {
	BaseEmitter, // BaseEmitterData: EmitterAsset = the base EmitterDocument
	Emitter, // EmitterData: EmitterAssets[] = child EmitterDocuments, one child instance per parent particle
	SpawnRate, // SpawnRateData
	SpawnPosition,
	SpawnDirection,
	SpawnSpeed,
	SpawnSize,
	SpawnRotation,
	SpawnRotationSpeed,
	SpawnOrientation,
	SpawnAnimationFrame,
	SpawnAnimation,
	UpdateAge,
	UpdateCameraProximity,
	AirResistance,
	Gravity,
	LocalForce,
	Turbulance, // BF3's spelling (TurbulanceData)
	UpdateLinearVelocity,
	UpdateRotation,
	UpdateSize,
	UpdateSizeX,
	UpdateSizeY,
	UpdateSizeZ,
	UpdateColor,
	UpdateTransparency,
	UpdateAlphaLevelMin,
	UpdateAlphaLevelMax,
	UpdateAlphaLevelScale,
	UpdateTextureCoords,
	UpdateOrientation,
	UpdateClipScale,
};
inline constexpr size_t kProcessorTypeCount = 32;
static_assert(size_t(ProcessorType::UpdateClipScale) + 1 == kProcessorTypeCount);

constexpr ProcessorStage processor_stage(ProcessorType p_type) {
	if (p_type <= ProcessorType::Emitter) {
		return ProcessorStage::Structural;
	}
	if (p_type == ProcessorType::SpawnRate) {
		return ProcessorStage::Emitter;
	}
	return p_type <= ProcessorType::SpawnAnimation ? ProcessorStage::Spawn : ProcessorStage::Update;
}

// "SpawnRateData" and back; nullopt for a name outside the table.
std::string_view processor_type_name(ProcessorType p_type);
std::optional<ProcessorType> processor_type_from_name(std::string_view p_ebx_type);

// ---- evaluators --------------------------------------------------------------------------------------------

// The 10 EvaluatorData types. A processor's Pre names at most one evaluator and no evaluator references another
// (PolynomialOperatorData holds its operands inline as PolynomialTempData): the evaluator "tree" is one leaf deep on
// all 3,410 emitters.
enum class EvaluatorType : uint8_t {
	Polynomial, // PolynomialData
	PolynomialColorInterp, // PolynomialColorInterpData
	PolynomialOperator, // PolynomialOperatorData (Multiplication / Addition of two inline polynomials)
	Random, // RandomEvaluatorData
	RandomXyz, // RandomXYZEvaluatorData
	Box, // BoxEvaluatorData
	Sphere, // SphereEvaluatorData
	RotateVector, // RotateVectorData
	CameraProximity, // CameraProximityEvaluatorData
	SampleTexture, // SampleTextureData
};
inline constexpr size_t kEvaluatorTypeCount = 10;
static_assert(size_t(EvaluatorType::SampleTexture) + 1 == kEvaluatorTypeCount);

std::string_view evaluator_type_name(EvaluatorType p_type);
std::optional<EvaluatorType> evaluator_type_from_name(std::string_view p_ebx_type);

// ProcessorData.EvaluatorInput: what t the evaluator reads (census: EfNormTime 36,806, EfOne 24,755,
// EfEmitterNormTime 2,136, EfVelocity 2 processors over all emitter partitions).
enum class EvaluatorInput : uint8_t {
	NormTime, // EfNormTime: particle age / UpdateAge.Lifetime, clamped to [0, 1]
	EmitterNormTime, // EfEmitterNormTime: emitter age / template Lifetime (0 for a continuous emitter)
	One, // EfOne: 1
	Velocity, // EfVelocity: |particle velocity| in m/s [inferred]
};
inline constexpr size_t kEvaluatorInputCount = 4;
std::string_view evaluator_input_name(EvaluatorInput p_input);
std::optional<EvaluatorInput> evaluator_input_from_name(std::string_view p_name);

// EvaluatorData.EmitterParameter: an instance-supplied channel (census: None 34,482, Distance 1,703, Vec 722,
// Param1 122, Param2 29, VecAverage 20, Param3 8). VFX1 carries it and evaluates as if None (docs §6.6, open Q3).
enum class EmitterParameter : uint8_t {
	None,
	Distance,
	Vec,
	VecAverage,
	Param1,
	Param2,
	Param3,
};
inline constexpr size_t kEmitterParameterCount = 7;
std::string_view emitter_parameter_name(EmitterParameter p_parameter);
std::optional<EmitterParameter> emitter_parameter_from_name(std::string_view p_name);

// PolynomialOperatorData.Operation (census: Multiplication 85, Addition 22).
enum class PolynomialOperation : uint8_t {
	Multiplication,
	Addition,
};
std::optional<PolynomialOperation> polynomial_operation_from_name(std::string_view p_name);

// EmitterTemplateData.EmittableType (census: ScreenAlignedQuad 1,899, ParticleMesh 931, DirectionAlignedQuad 558,
// Quad 18, WorldAlignedQuad 4; Trail and Point unused in MP but accepted).
enum class EmittableType : uint8_t {
	ScreenAlignedQuad,
	DirectionAlignedQuad,
	WorldAlignedQuad,
	Quad,
	ParticleMesh,
	Trail,
	Point,
};
inline constexpr size_t kEmittableTypeCount = 7;
std::string_view emittable_type_name(EmittableType p_type);
std::optional<EmittableType> emittable_type_from_name(std::string_view p_name);

// Quality tiers (EffectEntityData / EmitterEntityData HighEndPc, MediumPc, LowEndPc), as a bit mask.
enum TierBits : uint8_t {
	kTierLow = 1u << 0,
	kTierMedium = 1u << 1,
	kTierHigh = 1u << 2,
	kTierAll = kTierLow | kTierMedium | kTierHigh,
};

// ---- structural bounds (hostile data; docs §10) ------------------------------------------------------------
// Retail maxima (census 2026-10-15) are in the comments; the compiler refuses a def past any bound with an error.

inline constexpr uint32_t kMaxChainLength = 64; // reachable processors per chain (retail 26)
inline constexpr uint32_t kMaxBaseDepth = 4; // BaseEmitterData hops (retail 1, never nested)
inline constexpr uint32_t kMaxChildDepth = 2; // EmitterData nesting (retail 1: no child has children)
inline constexpr uint32_t kMaxChildrenPerEmitter = 16; // EmitterData.EmitterAssets (retail 4)
inline constexpr uint32_t kMaxEmitterMaxCount = 16384; // EmitterTemplateData.MaxCount (retail 1,051)
inline constexpr uint32_t kMaxEffectComponents = 256; // EffectEntityData.Components (retail 41)
inline constexpr uint32_t kMaxEmittersPerEffect = 512; // emitter defs incl. children and bases (retail 46)
inline constexpr uint32_t kMaxEffectParticles = 1u << 17; // sum of MaxCount, children x parent cap (retail 3,000 / 2,300)
inline constexpr uint32_t kMaxInstanceCount = 1024; // EffectEntityData.MaxInstanceCount (retail 100)
inline constexpr uint32_t kMaxGradientSamples = 4096; // SampleTextureData.GradientData (retail 128..512)
inline constexpr uint32_t kMaxClipLookup = 4096; // UpdateClipScaleData.Lookup int16s (retail 132 on every instance)
inline constexpr uint32_t kMaxZOcclusionLookup = 4096; // EmitterTemplateData.ZOcclusionLookup (retail 33)
inline constexpr float kMaxAbsParameter = 1.0e7f; // any compiled float (retail max |x| 7.6e6, one polynomial)
inline constexpr size_t kMaxNameLength = 1024; // partition and texture names
inline constexpr uint32_t kMaxFlipbookFrames = 1024; // EmitterTextureInfo.AnimationFrameCount (retail 0..49, columns 0..8)
inline constexpr float kMaxStartAge = 3600.0f; // EffectStart::start_age is clamped to this (simulator.h)

// ---- polynomials -------------------------------------------------------------------------------------------

// PolynomialData / PolynomialTempData: Coefficients (x, y, z, w) = (c3, c2, c1, c0), so
//   f(t) = ((x*t + y)*t + z)*t + w,  f(0) = w,  f(1) = x + y + z + w,
// then clamp(f, MinClamp, MaxClamp) * ScaleValue (plan §7.10 item 2; clamp-then-scale order [inferred]; the
// golden in fx.contract pins it). On 16,410 instances the non-zero (x, y, z) patterns are only z, yz, xyz or none:
// fitted degree 1/2/3 curves, so z is linear and x cubic. MinClamp > MaxClamp never occurs in retail; the clamp
// below then yields MaxClamp. Retail MinClamp == MaxClamp (a constant) occurs 137 times.
struct PolynomialDef {
	Vec4 coefficients{ 0, 0, 0, 1 }; // x = c3, y = c2, z = c1, w = c0
	float min_clamp = 0;
	float max_clamp = 1;
	float scale = 1;
	bool operator==(const PolynomialDef &) const = default;
};

// The raw cubic, before clamp and scale (Horner form, the operation order the goldens use).
inline float polynomial_raw(const Vec4 &p_c, float p_t) {
	return ((p_c.x * p_t + p_c.y) * p_t + p_c.z) * p_t + p_c.w;
}

inline float evaluate_polynomial(const PolynomialDef &p_poly, float p_t) {
	const float f = polynomial_raw(p_poly.coefficients, p_t);
	return std::fmin(std::fmax(f, p_poly.min_clamp), p_poly.max_clamp) * p_poly.scale;
}

// PolynomialColorInterpData: colour = lerp(Color1, Color0, clamp(f(t), 0, 1)), so f = 1 gives Color0 (plan §3.14;
// bright to dim on 689 of 846 directed instances). No MinClamp / MaxClamp / ScaleValue on this type.
inline Vec3 evaluate_color_interp(const Vec4 &p_coefficients, const Vec3 &p_color0, const Vec3 &p_color1, float p_t) {
	const float f = std::fmin(std::fmax(polynomial_raw(p_coefficients, p_t), 0.0f), 1.0f);
	return { p_color1.x + (p_color0.x - p_color1.x) * f, p_color1.y + (p_color0.y - p_color1.y) * f, p_color1.z + (p_color0.z - p_color1.z) * f };
}

// ---- random numbers ----------------------------------------------------------------------------------------
// Counter-based, so a draw depends only on its key, never on how many draws came before, which makes a run
// independent of thread count and item partition (docs §6.8). splitmix64 finaliser (public domain, Vigna).
//
// Key layout. Every random draw of the simulator is exactly one of these, so no two draws share a key:
//   per start and component (SpawnProbability)  fx_key(start seed, kKeyComponentDraw, component, 0)
//   a root emitter instance's seed               fx_emitter_seed(start seed, component, emitter index)
//   a child emitter instance's seed              fx_child_seed(parent instance seed, child emitter index, parent serial)
//   a particle draw                              fx_key(instance seed, particle serial, slot, lane)
// slot = the ProcessorType index (0..31), kSlotLight (the light's random intensity) or kSlotTurbulence; lane = the
// vector component (0..3) of a draw, or for turbulence knot * 3 + axis (docs §6.4). The emitter index and, for a
// child, the parent particle's serial are in the instance seed, so two emitters of one component, a parent and its
// children, and two children of one parent never draw the same numbers.

inline uint64_t fx_mix64(uint64_t p_x) {
	p_x += 0x9E3779B97F4A7C15ull;
	p_x = (p_x ^ (p_x >> 30)) * 0xBF58476D1CE4E5B9ull;
	p_x = (p_x ^ (p_x >> 27)) * 0x94D049BB133111EBull;
	return p_x ^ (p_x >> 31);
}

inline uint64_t fx_key(uint64_t p_seed, uint32_t p_a, uint32_t p_b, uint32_t p_c) {
	return fx_mix64(fx_mix64(fx_mix64(p_seed ^ p_a) ^ (uint64_t(p_b) << 1)) ^ (uint64_t(p_c) << 2));
}

inline constexpr uint32_t kKeyComponentDraw = 0xFFFF0001u;
inline constexpr uint32_t kKeyEmitterInstance = 0xFFFF0002u;
inline constexpr uint32_t kKeyChildInstance = 0xFFFF0003u;
inline constexpr uint32_t kSlotLight = 32;
inline constexpr uint32_t kSlotTurbulence = 33;

inline uint64_t fx_emitter_seed(uint64_t p_start_seed, uint32_t p_component, uint32_t p_emitter) {
	return fx_key(fx_mix64(p_start_seed ^ kKeyEmitterInstance), kKeyEmitterInstance, p_component, p_emitter);
}

inline uint64_t fx_child_seed(uint64_t p_parent_instance_seed, uint32_t p_child_emitter, uint32_t p_parent_serial) {
	return fx_key(fx_mix64(p_parent_instance_seed ^ kKeyChildInstance), kKeyChildInstance, p_child_emitter, p_parent_serial);
}

// The seed of start i of a multi-start harness run (`bf3dump fx simulate --starts K`, `fx check`; docs §7.7).
inline uint64_t fx_harness_seed(uint64_t p_seed, uint64_t p_start) {
	return fx_mix64(p_seed + p_start);
}

// Uniform in [0, 1): the top 24 bits, so every value is exact in a float and 1 is never returned.
inline float fx_unit(uint64_t p_key) {
	return float(p_key >> 40) * (1.0f / 16777216.0f);
}

// RandomEvaluatorData: lerp(Min, Max, u). Min > Max is legal data (126 retail instances, e.g. Min 1, Max -1).
inline float fx_uniform(float p_min, float p_max, float p_u) {
	return p_min + (p_max - p_min) * p_u;
}

} // namespace bf3::fx
