#pragma once

#include "bf3/core/math.h"
#include "bf3/core/util/splitmix64.h"

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
using Vec3 = core::Vec3;
struct Vec4 {
	float x = 0, y = 0, z = 0, w = 0;
	bool operator==(const Vec4 &) const = default;
};

using Transform = core::Transform;

enum class ProcessorStage : uint8_t {
	Structural,
	Emitter,
	Spawn,
	Update,
};

enum class ProcessorType : uint8_t {
	BaseEmitter,
	Emitter,
	SpawnRate,
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
	Turbulance,
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

std::string_view processor_type_name(ProcessorType p_type);
std::optional<ProcessorType> processor_type_from_name(std::string_view p_ebx_type);

enum class EvaluatorType : uint8_t {
	Polynomial,
	PolynomialColorInterp,
	PolynomialOperator,
	Random,
	RandomXyz,
	Box,
	Sphere,
	RotateVector,
	CameraProximity,
	SampleTexture,
};
inline constexpr size_t kEvaluatorTypeCount = 10;
static_assert(size_t(EvaluatorType::SampleTexture) + 1 == kEvaluatorTypeCount);

std::string_view evaluator_type_name(EvaluatorType p_type);
std::optional<EvaluatorType> evaluator_type_from_name(std::string_view p_ebx_type);

enum class EvaluatorInput : uint8_t {
	NormTime,
	EmitterNormTime,
	One,
	Velocity,
};
inline constexpr size_t kEvaluatorInputCount = 4;
std::string_view evaluator_input_name(EvaluatorInput p_input);
std::optional<EvaluatorInput> evaluator_input_from_name(std::string_view p_name);

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

enum class PolynomialOperation : uint8_t {
	Multiplication,
	Addition,
};
std::optional<PolynomialOperation> polynomial_operation_from_name(std::string_view p_name);

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

enum TierBits : uint8_t {
	kTierLow = 1u << 0,
	kTierMedium = 1u << 1,
	kTierHigh = 1u << 2,
	kTierAll = kTierLow | kTierMedium | kTierHigh,
};

inline constexpr uint32_t kMaxChainLength = 64;
inline constexpr uint32_t kMaxBaseDepth = 4;
inline constexpr uint32_t kMaxChildDepth = 2;
inline constexpr uint32_t kMaxChildrenPerEmitter = 16;
inline constexpr uint32_t kMaxEmitterMaxCount = 16384;
inline constexpr uint32_t kMaxEffectComponents = 256;
inline constexpr uint32_t kMaxEmittersPerEffect = 512;
inline constexpr uint32_t kMaxEffectParticles = 1u << 17;
inline constexpr uint32_t kMaxInstanceCount = 1024;
inline constexpr uint32_t kMaxGradientSamples = 4096;
inline constexpr uint32_t kMaxClipLookup = 4096;
inline constexpr uint32_t kMaxZOcclusionLookup = 4096;
inline constexpr float kMaxAbsParameter = 1.0e7f;
inline constexpr size_t kMaxNameLength = 1024;
inline constexpr uint32_t kMaxFlipbookFrames = 1024;
inline constexpr float kMaxStartAge = 3600.0f;

struct PolynomialDef {
	Vec4 coefficients{ 0, 0, 0, 1 };
	float min_clamp = 0;
	float max_clamp = 1;
	float scale = 1;
	bool operator==(const PolynomialDef &) const = default;
};

inline float polynomial_raw(const Vec4 &p_c, float p_t) {
	return ((p_c.x * p_t + p_c.y) * p_t + p_c.z) * p_t + p_c.w;
}

inline float evaluate_polynomial(const PolynomialDef &p_poly, float p_t) {
	const float f = polynomial_raw(p_poly.coefficients, p_t);
	return std::fmin(std::fmax(f, p_poly.min_clamp), p_poly.max_clamp) * p_poly.scale;
}

inline Vec3 evaluate_color_interp(const Vec4 &p_coefficients, const Vec3 &p_color0, const Vec3 &p_color1, float p_t) {
	const float f = std::fmin(std::fmax(polynomial_raw(p_coefficients, p_t), 0.0f), 1.0f);
	return { p_color1.x + (p_color0.x - p_color1.x) * f, p_color1.y + (p_color0.y - p_color1.y) * f, p_color1.z + (p_color0.z - p_color1.z) * f };
}

using core::splitmix64;

inline uint64_t fx_key(uint64_t p_seed, uint32_t p_a, uint32_t p_b, uint32_t p_c) {
	return splitmix64(splitmix64(splitmix64(p_seed ^ p_a) ^ (uint64_t(p_b) << 1)) ^ (uint64_t(p_c) << 2));
}

inline constexpr uint32_t kKeyComponentDraw = 0xFFFF0001u;
inline constexpr uint32_t kKeyEmitterInstance = 0xFFFF0002u;
inline constexpr uint32_t kKeyChildInstance = 0xFFFF0003u;
inline constexpr uint32_t kSlotLight = 32;
inline constexpr uint32_t kSlotTurbulence = 33;

inline uint64_t fx_emitter_seed(uint64_t p_start_seed, uint32_t p_component, uint32_t p_emitter) {
	return fx_key(splitmix64(p_start_seed ^ kKeyEmitterInstance), kKeyEmitterInstance, p_component, p_emitter);
}

inline uint64_t fx_child_seed(uint64_t p_parent_instance_seed, uint32_t p_child_emitter, uint32_t p_parent_serial) {
	return fx_key(splitmix64(p_parent_instance_seed ^ kKeyChildInstance), kKeyChildInstance, p_child_emitter,
			p_parent_serial);
}

inline uint64_t fx_harness_seed(uint64_t p_seed, uint64_t p_start) {
	return splitmix64(p_seed + p_start);
}

inline float fx_unit(uint64_t p_key) {
	return float(p_key >> 40) * (1.0f / 16777216.0f);
}

inline float fx_uniform(float p_min, float p_max, float p_u) {
	return p_min + (p_max - p_min) * p_u;
}

}
