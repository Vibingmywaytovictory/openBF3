#pragma once

#include "bf3/core/result.h"
#include "bf3/render/dxbc.h"

#include <string>
#include <string_view>

namespace bf3::render {

inline constexpr float kSpecularPowerScale = 10.0f;
inline constexpr float kSpecularPowerBias = 1.0f;
inline constexpr float kSpecularNormAdd = 8.0f;
inline constexpr float kSpecularNormScale = 0.125f;
inline constexpr float kEnvmapMipScale = 10.0f;
inline constexpr float kTerrainSmoothness = 0.5f;
inline constexpr std::string_view kSunSpecularScaleGlobal = "bf3_sun_specular_scale";
inline constexpr std::string_view kOutdoorLightProgram =
		"Dx11/DeferredOutdoorLight_shadow=0,trans=0,shadowQuality=0,transShadow=0,cloud=0,skyVis=0,msaa=1,gbLayout=0";

struct LightPassLobe {
	float power_scale = 0.0f;
	float power_bias = 0.0f;
	float norm_add = 0.0f;
	float norm_scale = 0.0f;
	float envmap_mip_scale = 0.0f;
};

core::Result<LightPassLobe> measure_light_pass(const dxbc::Program &p_program);

}
