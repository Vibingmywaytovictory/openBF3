#pragma once
// L1 look: the level's visual environment (sun, sky, ambient, fog, tonemap, colour correction) as BF3's
// VisualEnvironment EBX gives it, and its per-camera blend. Godot-free; read on the loader thread that plans the
// scene, blended on the main thread every frame (pure arithmetic over the read states). Port of the selection and
// area rules of IW4L level_bf3 environment.rs / look.rs (the user's Apache-2.0 code).
//
// Selection (README "L1 look"): a level places VisualEnvironmentReferenceObjectData, each a blueprint whose
// VisualEnvironmentEntityData lists the components. The reference's Priority orders them (the entity's own Priority
// is 1 on every Metro VE, which M1 read). A VE whose Visibility a PropertyConnection drives from an
// AreaProximityEntityData is an area: the proximity's LinkConnection names its shape (VolumeVectorShapeData polygon
// swept up by Height, or OBBData box), its ProximityDistance the fade. Every other VE that is not Excluded is
// always on. Per component (OutdoorLight, Enlighten, Sky, Fog, Tonemap, ColorCorrection): the base state takes it
// from the always-on VE with the highest reference Priority that has it enabled; then every area, lowest Priority
// first, blends the components it has over that by its weight at the camera (1 inside the shape, falling linearly
// to 0 at ProximityDistance outside it [inferred: AreaProximity semantics]).
//
// Metro (MP_Subway): park_01 (Priority 2750) is the base; city_01 (3500, the TDM street box), subway_01 (4000, the
// station polygon) and subway_02 (4500, sun off deeper in) are areas; cameras_01 is Excluded; motionblur has no
// component the look reads.
//
// Mapping to Godot (look_bridge.cpp in bf3_godot) keeps BF3's radiometric ratios instead of normalising colours:
//   * sun: DirectionalLight3D colour = SunColor / its peak, energy = the peak (park 4, city 6) [inferred: with
//     energy = peak / pi, i.e. SunColor read as irradiance, every view drew far darker than BF3 at any exposure in
//     Min..MaxExposure];
//   * ambient: the Enlighten sky box colours as radiance (park sky (0.06, 0.116, 0.157) against a sun of 4) for
//     the upper hemisphere, plus an estimate of Enlighten's sun bounce for the lower one and the horizon:
//     SunColor x sin(elevation) x kBounceAlbedo x BounceScale [inferred: Enlighten is
//     precomputed radiosity the install's res hold in a format not read here. Enlighten SunScale is not applied:
//     with it the city (SunScale 0.25) drew its shade near black at every exposure within Min..MaxExposure];
//   * sky: the PanoramicTexture over the upper hemisphere (u from PanoramicRotation, v over PanoramicUVMinY..MaxY),
//     blended by the PanoramicAlphaTexture over the SkyGradientTexture, times BrightnessScale;
//   * fog: Godot depth fog fitted to the Fog Curve over Start..End (fog_fit), its colour the sky's horizon (the
//     panorama's lowest rows times BrightnessScale) tinted toward FogColor by the peak of FogColorCurve (IW4L);
//   * exposure: BF3 exposes each frame to MiddleGray / average luminance, clamped to Min..MaxExposure; Godot's auto
//     exposure does the same (scale MiddleGray, its luminance clamp from the two exposure limits; the clamp's unit
//     was measured, look_bridge.cpp kLuminancePerIso). LookFrame::exposure is a static estimate of that formula (a
//     kReferenceAlbedo surface under the ambient, kSunlitShare of it in the sun) [inferred], drawn only with
//     --look-off=exposure;
//   * tonemap: Godot filmic (BF3 FilmicNeutral); bloom: Godot glow at the peak BloomScale;
//   * colour correction: Contrast, Brightness and Saturation per channel and the ColorGradingTexture (a 32^3
//     volume) baked into one 32^3 LUT per state (grade_lut), the dominant state's LUT drawn.
//
// The blue cast of M1: M1 normalised the Enlighten sky colour to a peak of 0.7 and drew it as the only ambient at
// energy 0.6 against a sun of energy 1.0 (SunColor x 0.25): shade got 42 % of the sun's level in pure sky blue
// (BF3: sky radiance (0.06, 0.116, 0.157) against SunColor 4, and warm bounce light on top). L1 keeps the sky at
// its radiance, the sun at SunColor, and adds the bounce estimate; what teal remains in shade is BF3's own grade
// (the park LUT maps display grey 0.27 to (0.07, 0.20, 0.22)).
//
// Hostile data: every list read is capped (kMaxVisualEnvironments, kMaxAreaPoints), textures are decoded only at
// bounded sizes (a mip of at most kHorizonWidth texels across; a LUT only as an exact 32^3 ARGB8888 volume), and
// every number is checked finite; nothing here allocates by a data-controlled size beyond those caps.

#include "bf3/core/ebx_db.h"
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

using LookVec3 = std::array<float, 3>;
using LookCubic = std::array<float, 4>; // x t^3 + y t^2 + z t + w

inline constexpr size_t kMaxVisualEnvironments = 64;
inline constexpr size_t kMaxAreaPoints = 256;
inline constexpr uint32_t kGradeLutSide = 32;
inline constexpr uint32_t kHorizonWidth = 256;
inline constexpr float kBounceAlbedo = 0.25f; // mean albedo the sun bounce estimate assumes [inferred]
inline constexpr float kReferenceAlbedo = 0.3f; // the static exposure estimate's mean albedo [inferred]
inline constexpr float kSunlitShare = 0.1f; // share of a view the static exposure estimate puts in the sun [inferred]

// One VE's components (an area holds only those its VE has).
struct LookEnvState {
	bool light = false, enlighten = false, sky = false, fog = false, tonemap = false, correction = false;
	// OutdoorLightComponentData
	LookVec3 sun_color{ 1.0f, 1.0f, 1.0f }; // radiometric (park (4, 3.024, 1.88))
	float sun_rotation_x = 200.0f; // azimuth, degrees from Frostbite +Z toward +X
	float sun_rotation_y = 35.0f; // elevation above the horizon, degrees
	LookVec3 outdoor_sky{}, outdoor_ground{};
	// EnlightenComponentData
	LookVec3 sky_color{ 0.06f, 0.1f, 0.15f }, ground_color{ 0.1f, 0.1f, 0.1f };
	float bounce_scale = 1.0f, enlighten_sun_scale = 1.0f;
	// SkyComponentData (texture names are DxTexture res names, lowercase; "" none)
	std::string panorama, panorama_alpha, gradient;
	float sky_brightness = 1.0f, sky_rotation = 0.0f, uv_min_y = 0.0f, uv_max_y = 1.0f;
	LookVec3 horizon{ 0.5f, 0.55f, 0.6f }; // panorama horizon colour (linear, before BrightnessScale), when decoded
	bool horizon_decoded = false;
	// FogComponentData
	float fog_start = 0.0f, fog_end = 0.0f, fog_multiplier = 1.0f;
	LookCubic fog_curve{ 0.0f, 0.0f, 1.0f, 0.0f }, fog_color_curve{};
	LookVec3 fog_color{};
	bool fog_color_enable = false;
	float fog_peak = 0.0f, fog_exponent = 1.0f, fog_fit_end = 1.0f; // fog_fit of fog_curve (Godot density / curve / end)
	float fog_tint = 0.0f; // peak of FogColorCurve (0..1): FogColor's share of the fog colour
	// TonemapComponentData
	float middle_gray = 0.18f, min_exposure = 1.0f, max_exposure = 1.0f, exposure_time = 0.5f;
	LookVec3 bloom{};
	// ColorCorrectionComponentData
	LookVec3 brightness{ 1.0f, 1.0f, 1.0f }, contrast{ 1.0f, 1.0f, 1.0f }, saturation{ 1.0f, 1.0f, 1.0f };
	std::string grade_cube; // ColorGradingTexture res name ("" none / disabled)
	int32_t lut = -1; // index into LookEnvironment::luts (the state's baked correction), -1 identity
	std::string source; // the VE blueprint each component came from (report)
};

struct LookArea {
	std::string name; // VE blueprint
	int64_t priority = 0;
	bool box = false;
	// Polygon: outline (x, z) swept from floor up by height. Box: world -> box rows (unit axes) and half extents.
	std::vector<std::array<float, 2>> outline;
	float floor = 0.0f, height = 0.0f;
	std::array<LookVec3, 3> box_axes{};
	LookVec3 box_center{}, box_half{};
	float fade = 0.0f; // ProximityDistance
	LookEnvState state;
};

struct LookEnvironment {
	bool from_data = false;
	LookEnvState base;
	std::vector<LookArea> areas; // ascending priority
	std::vector<std::vector<uint8_t>> luts; // kGradeLutSide^3 RGBA8, red fastest, Godot's texel-centre convention
	std::vector<std::string> notes; // what was read / skipped (report)
};

// Reads the environment (no textures). p_world's VE objects; fails only on the world's budget error.
core::Result<LookEnvironment> read_look_environment(const core::EbxResolver &p_db, const core::level::World &p_world);
// Decodes what the bridge needs from textures on the loader thread: each state's panorama horizon colour and the
// baked LUTs. Failures become notes (the defaults stay).
void decode_look_textures(const core::LevelMount &p_mount, LookEnvironment &r_env);

// The blended values for one camera position, ready for the bridge.
struct LookFrame {
	LookVec3 sun_direction{ 0.0f, -1.0f, 0.0f }; // pointing from the sun, normalised
	LookVec3 sun_color{ 1.0f, 1.0f, 1.0f }; // peak 1
	float sun_energy = 1.0f;
	LookVec3 ambient_up{}, ambient_horizon{}, ambient_down{}; // radiance
	float sky_brightness = 1.0f, sky_rotation = 0.0f, uv_min_y = 0.0f, uv_max_y = 1.0f;
	LookVec3 horizon{}; // linear, BrightnessScale applied
	bool fog = false;
	float fog_begin = 0.0f, fog_end = 0.0f, fog_density = 0.0f, fog_curve = 1.0f;
	LookVec3 fog_color{};
	float middle_gray = 0.18f, min_exposure = 1.0f, max_exposure = 1.0f, exposure_speed = 2.0f;
	float exposure = 1.0f; // the static exposure (Godot tonemap_exposure)
	float bloom = 0.0f;
	int32_t lut = -1; // the dominant state's LUT
};

// Weight of an area at p_position (Frostbite space): 1 inside, linear to 0 at fade outside.
float look_area_weight(const LookArea &p_area, const LookVec3 &p_position);
LookFrame look_frame(const LookEnvironment &p_env, const LookVec3 &p_position);
LookFrame look_frame_of(const LookEnvState &p_state);

// Least-squares fit of Godot's depth fog, density x smoothstep(0, end, t)^curve, to BF3's Fog Curve (a cubic in
// t = (distance - Start) / (End - Start), clamped to 0..1) over t in 0..p_reach (the near part a level's views
// see): r_end is Godot's end as a fraction of BF3's span (1/64..1), r_density BF3's value there, r_curve the
// exponent (0.05..16). [inferred] Godot's depth fog as density x pow(smoothstep(begin, end, d), curve). Fixed
// work (64 x 65 x 49 evaluations, no data sizes).
void fog_fit(const LookCubic &p_curve, float p_reach, float &r_density, float &r_curve, float &r_end);
float look_cubic(const LookCubic &p_c, float p_t);
// BF3 colour correction (contrast about 0.5, brightness, saturation about Rec.601 luma, per channel) of a display
// colour, then the grading cube (kGradeLutSide^3 RGBA8, red fastest, corner-aligned as BF3 samples it; may be empty).
LookVec3 look_grade(const LookEnvState &p_state, const std::vector<uint8_t> &p_cube, const LookVec3 &p_display);

} // namespace bf3::render
