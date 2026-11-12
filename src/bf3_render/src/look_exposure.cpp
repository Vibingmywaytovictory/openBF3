#include "bf3/render/look_exposure.h"

#include "bf3/core/util/format.h"

#include <algorithm>
#include <cmath>

namespace bf3::render {

LookExposure look_exposure_of(const LookFrame &p_frame) {
	return LookExposure{ p_frame.middle_gray, p_frame.min_exposure, p_frame.max_exposure, p_frame.exposure_time };
}

float exposure_luminance(const LookVec3 &p_mean) {
	return kExposureLuminanceWeights[0] * p_mean[0] + kExposureLuminanceWeights[1] * p_mean[1] +
			kExposureLuminanceWeights[2] * p_mean[2];
}

bool exposure_luminance_valid(float p_luminance) {
	return p_luminance >= kExposureMinLuminance && p_luminance <= kExposureMaxLuminance;
}

float exposure_target(float p_luminance, const LookExposure &p_exposure) {
	return std::clamp(p_exposure.middle_gray / (p_luminance + kExposureLuminanceEpsilon), p_exposure.min_exposure,
			std::max(p_exposure.min_exposure, p_exposure.max_exposure));
}

ExposureState exposure_step(
		const ExposureState &p_previous, float p_luminance, const LookExposure &p_exposure, float p_dt, bool p_reset) {
	if (!exposure_luminance_valid(p_luminance)) {
		return p_previous;
	}
	const float target = exposure_target(p_luminance, p_exposure);
	const bool blend = p_exposure.adjust_time > kExposureMinAdjustTime && !p_reset && p_previous.measured;
	ExposureState out;
	out.exposure = blend ? p_previous.exposure +
					(target - p_previous.exposure) * std::min(std::max(p_dt, 0.0f) / p_exposure.adjust_time, 1.0f)
						 : target;
	out.luminance = p_luminance;
	out.measured = true;
	return out;
}

std::string exposure_step_glsl() {
	return core::fmt("const vec3 kExposureWeights = vec3(%.9g, %.9g, %.9g);\n"
					 "vec4 exposure_step(vec4 previous, vec3 mean, float middle_gray, float min_exposure, float "
					 "max_exposure, float adjust_time, float dt, bool reset) {\n"
					 "\tfloat luminance = dot(mean, kExposureWeights);\n"
					 "\tif (!(luminance >= %.9g && luminance <= %.9g)) {\n"
					 "\t\treturn previous;\n"
					 "\t}\n"
					 "\tfloat target = clamp(middle_gray / (luminance + %.9g), min_exposure, max(min_exposure, "
					 "max_exposure));\n"
					 "\tbool blend = adjust_time > %.9g && !reset && previous.z > 0.5;\n"
					 "\tfloat exposure = blend ? previous.x + (target - previous.x) * min(max(dt, 0.0) / adjust_time, "
					 "1.0) : target;\n"
					 "\treturn vec4(exposure, luminance, 1.0, previous.w);\n"
					 "}\n",
			double(kExposureLuminanceWeights[0]), double(kExposureLuminanceWeights[1]),
			double(kExposureLuminanceWeights[2]), double(kExposureMinLuminance), double(kExposureMaxLuminance),
			double(kExposureLuminanceEpsilon), double(kExposureMinAdjustTime));
}

}
