#pragma once

#include "bf3/render/look_environment.h"

#include <array>
#include <string>

namespace bf3::render {

inline constexpr std::array<float, 3> kExposureLuminanceWeights{ 0.30f, 0.59f, 0.11f };
inline constexpr float kExposureLuminanceEpsilon = 0.001f;
inline constexpr float kExposureMinLuminance = 0.00001f;
inline constexpr float kExposureMaxLuminance = 10000.0f;
inline constexpr float kExposureMinAdjustTime = 0.01f;

struct LookExposure {
	float middle_gray = 0.18f;
	float min_exposure = 1.0f;
	float max_exposure = 1.0f;
	float adjust_time = 0.5f;
};

struct ExposureState {
	float exposure = 1.0f;
	float luminance = 0.0f;
	bool measured = false;
};

LookExposure look_exposure_of(const LookFrame &p_frame);
float exposure_luminance(const LookVec3 &p_mean);
bool exposure_luminance_valid(float p_luminance);
float exposure_target(float p_luminance, const LookExposure &p_exposure);
ExposureState exposure_step(
		const ExposureState &p_previous, float p_luminance, const LookExposure &p_exposure, float p_dt, bool p_reset);

std::string exposure_step_glsl();

}
