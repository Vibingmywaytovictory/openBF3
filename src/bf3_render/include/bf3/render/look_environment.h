#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/level/area_volume.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf3::core {
class LevelMount;
}
namespace bf3::core::level {
class World;
}

namespace bf3::render {

using LookVec3 = core::Vec3;
using LookCubic = std::array<float, 4>;

inline constexpr size_t kMaxVisualEnvironments = 64;
inline constexpr uint32_t kGradeLutSide = 32;
inline constexpr uint32_t kHorizonWidth = 256;
inline constexpr float kBounceAlbedo = 0.25f;
inline constexpr float kReferenceAlbedo = 0.3f;
inline constexpr float kSunlitShare = 0.1f;

struct LookEnvState {
	bool light = false, enlighten = false, sky = false, fog = false, tonemap = false, correction = false;

	LookVec3 sun_color{ 1.0f, 1.0f, 1.0f };
	float sun_rotation_x = 200.0f;
	float sun_rotation_y = 35.0f;
	LookVec3 outdoor_sky{}, outdoor_ground{};

	LookVec3 sky_color{ 0.06f, 0.1f, 0.15f }, ground_color{ 0.1f, 0.1f, 0.1f };
	float bounce_scale = 1.0f, enlighten_sun_scale = 1.0f;

	std::string panorama, panorama_alpha, gradient;
	float sky_brightness = 1.0f, sky_rotation = 0.0f, uv_min_y = 0.0f, uv_max_y = 1.0f;
	LookVec3 horizon{ 0.5f, 0.55f, 0.6f };
	bool horizon_decoded = false;

	float fog_start = 0.0f, fog_end = 0.0f, fog_multiplier = 1.0f;
	LookCubic fog_curve{ 0.0f, 0.0f, 1.0f, 0.0f }, fog_color_curve{};
	LookVec3 fog_color{};
	bool fog_color_enable = false;
	float fog_peak = 0.0f, fog_exponent = 1.0f, fog_fit_end = 1.0f;
	float fog_tint = 0.0f;

	float middle_gray = 0.18f, min_exposure = 1.0f, max_exposure = 1.0f, exposure_time = 0.5f;
	LookVec3 bloom{};

	LookVec3 brightness{ 1.0f, 1.0f, 1.0f }, contrast{ 1.0f, 1.0f, 1.0f }, saturation{ 1.0f, 1.0f, 1.0f };
	std::string grade_cube;
	int32_t lut = -1;
	std::string source;
};

struct LookArea {
	std::string name;
	int64_t priority = 0;
	core::level::AreaVolume volume;
	float fade = 0.0f;
	LookEnvState state;
};

struct LookEnvironment {
	bool from_data = false;
	LookEnvState base;
	std::vector<LookArea> areas;
	std::vector<std::vector<uint8_t>> luts;
	std::vector<std::string> notes;
};

core::Result<LookEnvironment> read_look_environment(const core::EbxResolver &p_db, const core::level::World &p_world);

void decode_look_textures(const core::LevelMount &p_mount, LookEnvironment &r_env);

struct LookFrame {
	LookVec3 sun_direction{ 0.0f, -1.0f, 0.0f };
	LookVec3 sun_color{ 1.0f, 1.0f, 1.0f };
	float sun_energy = 1.0f;
	LookVec3 ambient_up{}, ambient_horizon{}, ambient_down{};
	float sky_brightness = 1.0f, sky_rotation = 0.0f, uv_min_y = 0.0f, uv_max_y = 1.0f;
	LookVec3 horizon{};
	bool fog = false;
	float fog_begin = 0.0f, fog_end = 0.0f, fog_density = 0.0f, fog_curve = 1.0f;
	LookVec3 fog_color{};
	float middle_gray = 0.18f, min_exposure = 1.0f, max_exposure = 1.0f, exposure_speed = 2.0f;
	float exposure = 1.0f;
	float bloom = 0.0f;
	int32_t lut = -1;
};

float look_area_weight(const LookArea &p_area, const LookVec3 &p_position);
LookFrame look_frame(const LookEnvironment &p_env, const LookVec3 &p_position);
LookFrame look_frame_of(const LookEnvState &p_state);

void fog_fit(const LookCubic &p_curve, float p_reach, float &r_density, float &r_curve, float &r_end);
float look_cubic(const LookCubic &p_c, float p_t);

LookVec3 look_grade(const LookEnvState &p_state, const std::vector<uint8_t> &p_cube, const LookVec3 &p_display);

}
