// bf3_fx name tables (fx_types.h): the EBX type and enumerator names of the emitter graphs, both ways. These tables
// are the vocabulary the census gate measures: a name outside them is "unknown" (docs/design/m2a-vfx1.md §6.1).
#include "bf3/fx/fx_types.h"

#include <array>
#include <string_view>

namespace bf3::fx {
namespace {

constexpr std::array<std::string_view, kProcessorTypeCount> kProcessorNames = {
	"BaseEmitterData",
	"EmitterData",
	"SpawnRateData",
	"SpawnPositionData",
	"SpawnDirectionData",
	"SpawnSpeedData",
	"SpawnSizeData",
	"SpawnRotationData",
	"SpawnRotationSpeedData",
	"SpawnOrientationData",
	"SpawnAnimationFrameData",
	"SpawnAnimationData",
	"UpdateAgeData",
	"UpdateCameraProximityData",
	"AirResistanceData",
	"GravityData",
	"LocalForceData",
	"TurbulanceData",
	"UpdateLinearVelocityData",
	"UpdateRotationData",
	"UpdateSizeData",
	"UpdateSizeXData",
	"UpdateSizeYData",
	"UpdateSizeZData",
	"UpdateColorData",
	"UpdateTransparencyData",
	"UpdateAlphaLevelMinData",
	"UpdateAlphaLevelMaxData",
	"UpdateAlphaLevelScaleData",
	"UpdateTextureCoordsData",
	"UpdateOrientationData",
	"UpdateClipScaleData",
};

constexpr std::array<std::string_view, kEvaluatorTypeCount> kEvaluatorNames = {
	"PolynomialData",
	"PolynomialColorInterpData",
	"PolynomialOperatorData",
	"RandomEvaluatorData",
	"RandomXYZEvaluatorData",
	"BoxEvaluatorData",
	"SphereEvaluatorData",
	"RotateVectorData",
	"CameraProximityEvaluatorData",
	"SampleTextureData",
};

constexpr std::array<std::string_view, kEvaluatorInputCount> kInputNames = {
	"EfNormTime",
	"EfEmitterNormTime",
	"EfOne",
	"EfVelocity",
};

constexpr std::array<std::string_view, kEmitterParameterCount> kParameterNames = {
	"EmitterParameterNone",
	"EmitterParameterDistance",
	"EmitterParameterVec",
	"EmitterParameterVecAverage",
	"EmitterParameter1",
	"EmitterParameter2",
	"EmitterParameter3",
};

constexpr std::array<std::string_view, kEmittableTypeCount> kEmittableNames = {
	"ScreenAlignedQuad",
	"DirectionAlignedQuad",
	"WorldAlignedQuad",
	"Quad",
	"ParticleMesh",
	"Trail",
	"Point",
};

template <class E, size_t N>
std::optional<E> find_name(const std::array<std::string_view, N> &p_table, std::string_view p_name) {
	for (size_t i = 0; i < N; ++i) {
		if (p_table[i] == p_name) {
			return E(i);
		}
	}
	return std::nullopt;
}

template <size_t N>
std::string_view name_at(const std::array<std::string_view, N> &p_table, size_t p_index) {
	return p_index < N ? p_table[p_index] : std::string_view();
}

} // namespace

std::string_view processor_type_name(ProcessorType p_type) {
	return name_at(kProcessorNames, size_t(p_type));
}
std::optional<ProcessorType> processor_type_from_name(std::string_view p_ebx_type) {
	return find_name<ProcessorType>(kProcessorNames, p_ebx_type);
}

std::string_view evaluator_type_name(EvaluatorType p_type) {
	return name_at(kEvaluatorNames, size_t(p_type));
}
std::optional<EvaluatorType> evaluator_type_from_name(std::string_view p_ebx_type) {
	return find_name<EvaluatorType>(kEvaluatorNames, p_ebx_type);
}

std::string_view evaluator_input_name(EvaluatorInput p_input) {
	return name_at(kInputNames, size_t(p_input));
}
std::optional<EvaluatorInput> evaluator_input_from_name(std::string_view p_name) {
	return find_name<EvaluatorInput>(kInputNames, p_name);
}

std::string_view emitter_parameter_name(EmitterParameter p_parameter) {
	return name_at(kParameterNames, size_t(p_parameter));
}
std::optional<EmitterParameter> emitter_parameter_from_name(std::string_view p_name) {
	return find_name<EmitterParameter>(kParameterNames, p_name);
}

std::optional<PolynomialOperation> polynomial_operation_from_name(std::string_view p_name) {
	if (p_name == "Multiplication") {
		return PolynomialOperation::Multiplication;
	}
	if (p_name == "Addition") {
		return PolynomialOperation::Addition;
	}
	return std::nullopt;
}

std::string_view emittable_type_name(EmittableType p_type) {
	return name_at(kEmittableNames, size_t(p_type));
}
std::optional<EmittableType> emittable_type_from_name(std::string_view p_name) {
	return find_name<EmittableType>(kEmittableNames, p_name);
}

} // namespace bf3::fx
