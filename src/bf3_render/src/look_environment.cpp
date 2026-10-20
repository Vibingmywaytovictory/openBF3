#include "bf3/render/look_environment.h"

#include "bf3/core/ebx.h"
#include "bf3/core/level/connections.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/res/res_loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace bf3::render {

using core::EbxInstance;
using core::EbxResolver;
using core::EbxStruct;
using core::EbxValue;
using core::level::LevelTransform;
using core::level::ObjectKind;
using core::level::PlacedObject;
using core::level::World;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr size_t kMaxComponents = 256;
constexpr uint32_t kFogFitSamples = 48;
constexpr uint32_t kFogFitSteps = 128;
constexpr float kFogFitReach = 1500.0f;
constexpr float kHorizonRows = 0.15f;

std::string lower(std::string_view p_s) {
	std::string out(p_s);
	for (char &c : out) {
		c = char(std::tolower(uint8_t(c)));
	}
	return out;
}

float max3(const LookVec3 &p_v) {
	return std::fmax(std::fmax(p_v[0], p_v[1]), p_v[2]);
}

bool finite3(const LookVec3 &p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]);
}

LookVec3 scaled(const LookVec3 &p_v, float p_s) {
	return { p_v[0] * p_s, p_v[1] * p_s, p_v[2] * p_s };
}

LookVec3 added(const LookVec3 &p_a, const LookVec3 &p_b) {
	return { p_a[0] + p_b[0], p_a[1] + p_b[1], p_a[2] + p_b[2] };
}

LookVec3 lerp3(const LookVec3 &p_a, const LookVec3 &p_b, float p_t) {
	return { p_a[0] + (p_b[0] - p_a[0]) * p_t, p_a[1] + (p_b[1] - p_a[1]) * p_t, p_a[2] + (p_b[2] - p_a[2]) * p_t };
}

float lerp1(float p_a, float p_b, float p_t) {
	return p_a + (p_b - p_a) * p_t;
}

std::optional<float> number(const EbxInstance &p_c, std::string_view p_field) {
	const std::optional<EbxValue> v = p_c.get(p_field);
	const std::optional<float> f = v ? v->as_f32() : std::nullopt;
	return f && std::isfinite(*f) ? f : std::nullopt;
}

std::optional<LookVec3> vec3(const EbxInstance &p_c, std::string_view p_field) {
	const std::optional<EbxValue> v = p_c.get(p_field);
	std::optional<std::array<float, 3>> out = v ? v->as_vec3() : std::nullopt;
	return out && finite3(*out) ? out : std::nullopt;
}

std::optional<LookCubic> cubic(const EbxInstance &p_c, std::string_view p_field) {
	const std::optional<EbxValue> v = p_c.get(p_field);
	if (!v) {
		return std::nullopt;
	}
	LookCubic out{};
	const char *axes[4] = { "x", "y", "z", "w" };
	for (size_t a = 0; a < 4; ++a) {
		const std::optional<EbxValue> c = v->get(axes[a]);
		const std::optional<float> f = c ? c->as_f32() : std::nullopt;
		if (!f || !std::isfinite(*f)) {
			return std::nullopt;
		}
		out[a] = *f;
	}
	return out;
}

bool flag(const EbxInstance &p_c, std::string_view p_field, bool p_default) {
	const std::optional<EbxValue> v = p_c.get(p_field);
	const std::optional<bool> b = v ? v->as_bool() : std::nullopt;
	return b.value_or(p_default);
}

std::string partition_name(const EbxResolver &p_db, const EbxInstance &p_instance) {
	const std::optional<std::string_view> n = p_db.name_of(p_instance.partition().guid());
	return n ? lower(*n) : std::string();
}

std::string texture_name(const EbxResolver &p_db, const EbxInstance &p_c, std::string_view p_field) {
	const std::optional<EbxInstance> t = p_db.follow(p_c.data(), p_field);
	return t ? partition_name(p_db, *t) : std::string();
}

bool read_component(const EbxResolver &p_db, const EbxInstance &p_c, const std::string &p_source, LookEnvState &r_state) {
	if (!flag(p_c, "Enable", true)) {
		return false;
	}
	if (p_c.is_a("OutdoorLightComponentData")) {
		if (r_state.light) {
			return false;
		}
		r_state.light = true;
		r_state.sun_color = vec3(p_c, "SunColor").value_or(r_state.sun_color);
		r_state.sun_rotation_x = number(p_c, "SunRotationX").value_or(r_state.sun_rotation_x);
		r_state.sun_rotation_y = number(p_c, "SunRotationY").value_or(r_state.sun_rotation_y);
		r_state.outdoor_sky = vec3(p_c, "SkyColor").value_or(r_state.outdoor_sky);
		r_state.outdoor_ground = vec3(p_c, "GroundColor").value_or(r_state.outdoor_ground);
	} else if (p_c.is_a("EnlightenComponentData")) {
		if (r_state.enlighten || !flag(p_c, "SkyBoxEnable", true)) {
			return false;
		}
		r_state.enlighten = true;
		r_state.sky_color = vec3(p_c, "SkyBoxSkyColor").value_or(r_state.sky_color);
		r_state.ground_color = vec3(p_c, "SkyBoxGroundColor").value_or(r_state.ground_color);
		r_state.bounce_scale = number(p_c, "BounceScale").value_or(1.0f);
		r_state.enlighten_sun_scale = number(p_c, "SunScale").value_or(1.0f);
	} else if (p_c.is_a("SkyComponentData")) {
		if (r_state.sky) {
			return false;
		}
		r_state.sky = true;
		r_state.panorama = texture_name(p_db, p_c, "PanoramicTexture");
		r_state.panorama_alpha = texture_name(p_db, p_c, "PanoramicAlphaTexture");
		r_state.gradient = texture_name(p_db, p_c, "SkyGradientTexture");
		r_state.sky_brightness = number(p_c, "BrightnessScale").value_or(1.0f);
		r_state.sky_rotation = number(p_c, "PanoramicRotation").value_or(0.0f);
		r_state.uv_min_y = number(p_c, "PanoramicUVMinY").value_or(0.0f);
		r_state.uv_max_y = number(p_c, "PanoramicUVMaxY").value_or(1.0f);
		if (!(r_state.uv_max_y > r_state.uv_min_y + 1e-3f)) {
			r_state.uv_min_y = 0.0f;
			r_state.uv_max_y = 1.0f;
		}
	} else if (p_c.is_a("FogComponentData")) {
		if (r_state.fog) {
			return false;
		}
		const std::optional<float> start = number(p_c, "Start"), end = number(p_c, "End");
		const float multiplier = number(p_c, "FogDistanceMultiplier").value_or(1.0f);
		if (!start || !end || !(*end > *start) || *start < 0.0f || !(multiplier > 0.0f)) {
			return false;
		}
		r_state.fog = true;
		r_state.fog_start = *start;
		r_state.fog_end = *end;
		r_state.fog_multiplier = multiplier;
		r_state.fog_curve = cubic(p_c, "Curve").value_or(LookCubic{ 0.0f, 0.0f, 1.0f, 0.0f });
		r_state.fog_color = vec3(p_c, "FogColor").value_or(LookVec3{});
		r_state.fog_color_enable = flag(p_c, "FogColorEnable", false);
		r_state.fog_color_curve = cubic(p_c, "FogColorCurve").value_or(LookCubic{});
		fog_fit(r_state.fog_curve, kFogFitReach / (*end - *start), r_state.fog_peak, r_state.fog_exponent, r_state.fog_fit_end);
		float tint = 0.0f;
		if (r_state.fog_color_enable && max3(r_state.fog_color) > 1e-6f) {
			for (uint32_t s = 0; s <= 256; ++s) {
				tint = std::fmax(tint, look_cubic(r_state.fog_color_curve, float(s) / 256.0f));
			}
		}
		r_state.fog_tint = std::clamp(tint, 0.0f, 1.0f);
	} else if (p_c.is_a("TonemapComponentData")) {
		if (r_state.tonemap) {
			return false;
		}
		r_state.tonemap = true;
		r_state.middle_gray = std::clamp(number(p_c, "MiddleGray").value_or(0.18f), 0.001f, 10.0f);
		r_state.min_exposure = std::clamp(number(p_c, "MinExposure").value_or(1.0f), 0.001f, 1000.0f);
		r_state.max_exposure = std::clamp(number(p_c, "MaxExposure").value_or(1.0f), r_state.min_exposure, 1000.0f);
		r_state.exposure_time = std::clamp(number(p_c, "ExposureAdjustTime").value_or(0.5f), 0.01f, 60.0f);
		r_state.bloom = vec3(p_c, "BloomScale").value_or(LookVec3{});
	} else if (p_c.is_a("ColorCorrectionComponentData")) {
		if (r_state.correction) {
			return false;
		}
		r_state.correction = true;
		r_state.brightness = vec3(p_c, "Brightness").value_or(LookVec3{ 1.0f, 1.0f, 1.0f });
		r_state.contrast = vec3(p_c, "Contrast").value_or(LookVec3{ 1.0f, 1.0f, 1.0f });
		r_state.saturation = vec3(p_c, "Saturation").value_or(LookVec3{ 1.0f, 1.0f, 1.0f });
		r_state.grade_cube = flag(p_c, "ColorGradingEnable", false) ? texture_name(p_db, p_c, "ColorGradingTexture") : std::string();
	} else {
		return false;
	}
	r_state.source += (r_state.source.empty() ? "" : " ") + std::string(p_c.type_name()) + "<" + p_source;
	return true;
}

struct Ve {
	const PlacedObject *object = nullptr;
	EbxInstance reference;
	int64_t priority = 0;
	std::string name;
	std::optional<EbxInstance> proximity;
	bool switched = false;
};

std::optional<LookArea> area_shape(const World &p_world, uint32_t p_scope, const EbxInstance &p_proximity) {
	const core::level::Connections &connections = p_world.scopes.connections(p_scope);
	std::optional<EbxInstance> polygon, box;
	for (const core::level::Connection &link : connections.link) {
		if (!(link.source == p_proximity)) {
			continue;
		}
		if (!polygon && link.target.is_a("VolumeVectorShapeData")) {
			polygon = link.target;
		} else if (!box && link.target.is_a("OBBData")) {
			box = link.target;
		}
	}
	LookArea out;
	out.fade = std::clamp(number(p_proximity, "ProximityDistance").value_or(0.0f), 0.0f, 1000.0f);
	if (polygon) {
		const PlacedObject *placed = p_world.placed(p_scope, *polygon);
		const LevelTransform t = placed != nullptr ? placed->transform : LevelTransform::identity();
		const std::optional<EbxValue> points = polygon->get("Points");
		if (points) {
			size_t n = 0;
			for (const EbxValue p : *points->as_array()) {
				if (++n > kMaxAreaPoints) {
					break;
				}
				const std::optional<std::array<float, 3>> local = p.as_vec3();
				if (!local || !finite3(*local)) {
					continue;
				}
				const LookVec3 world = t.transform_point(*local);
				if (out.outline.empty()) {
					out.floor = world[1];
				}
				out.floor = std::fmin(out.floor, world[1]);
				out.outline.push_back({ world[0], world[2] });
			}
		}
		out.height = std::clamp(number(*polygon, "Height").value_or(0.0f) * t.scale, 0.0f, 100000.0f);
		if (out.outline.size() >= 3 && out.height > 0.0f) {
			return out;
		}
		out.outline.clear();
	}
	if (box) {
		const PlacedObject *placed = p_world.placed(p_scope, *box);
		LevelTransform t;
		if (placed != nullptr) {
			t = placed->transform;
		} else if (const std::optional<EbxValue> rows = box->get("Transform"); rows && rows->as_transform()) {
			t = LevelTransform::from_rows(*rows->as_transform());
		}
		const std::optional<LookVec3> half = vec3(*box, "HalfExtents");
		if (!half || !((*half)[0] > 0.0f && (*half)[1] > 0.0f && (*half)[2] > 0.0f)) {
			return std::nullopt;
		}
		out.box = true;
		out.box_center = t.trans;
		const LookVec3 axes[3] = { t.right, t.up, t.forward };
		for (size_t a = 0; a < 3; ++a) {
			const float len = std::sqrt(axes[a][0] * axes[a][0] + axes[a][1] * axes[a][1] + axes[a][2] * axes[a][2]);
			if (!(len > 1e-6f) || !std::isfinite(len)) {
				return std::nullopt;
			}
			out.box_axes[a] = scaled(axes[a], 1.0f / len);
			out.box_half[a] = (*half)[a] * len * t.scale;
		}
		return out;
	}
	return std::nullopt;
}

double srgb_to_linear(double p_c) {
	return p_c <= 0.04045 ? p_c / 12.92 : std::pow((p_c + 0.055) / 1.055, 2.4);
}

std::optional<LookVec3> horizon_of(const core::LevelMount &p_mount, const std::string &p_name, std::string &r_note) {
	core::Result<core::res::LoadedTexture> t = core::res::load_texture(p_mount, p_name);
	if (!t) {
		r_note = p_name + ": " + t.error().message;
		return std::nullopt;
	}
	std::vector<core::res::MipSlice> mips;
	if (const core::res::ResError e = t->mips(mips); !e.ok()) {
		r_note = p_name + ": " + e.message();
		return std::nullopt;
	}
	const core::res::MipSlice *pick = nullptr;
	for (const core::res::MipSlice &m : mips) {
		if (m.face == 0 && m.width <= kHorizonWidth && m.width >= 4 && m.height >= 4 && (pick == nullptr || m.width > pick->width)) {
			pick = &m;
		}
	}
	std::vector<std::array<float, 4>> texels;
	if (pick == nullptr || !pick->texels(t->texture.format, texels) || texels.size() != size_t(pick->width) * pick->height) {
		r_note = p_name + ": no decodable mip of at most " + std::to_string(kHorizonWidth) + " texels across";
		return std::nullopt;
	}
	const bool srgb = t->texture.is_srgb();
	const uint32_t rows = std::max<uint32_t>(1, uint32_t(float(pick->height) * kHorizonRows));
	double sum[3] = { 0, 0, 0 };
	size_t n = 0;
	for (uint32_t y = pick->height - rows; y < pick->height; ++y) {
		for (uint32_t x = 0; x < pick->width; ++x) {
			const std::array<float, 4> &c = texels[size_t(y) * pick->width + x];
			for (int k = 0; k < 3; ++k) {
				sum[k] += srgb ? srgb_to_linear(c[size_t(k)]) : double(c[size_t(k)]);
			}
			++n;
		}
	}
	return LookVec3{ float(sum[0] / double(n)), float(sum[1] / double(n)), float(sum[2] / double(n)) };
}

std::vector<uint8_t> grade_cube_of(const core::LevelMount &p_mount, const std::string &p_name, std::string &r_note) {
	core::Result<core::res::LoadedTexture> t = core::res::load_texture(p_mount, p_name);
	if (!t) {
		r_note = p_name + ": " + t.error().message;
		return {};
	}
	const core::res::DxTexture &h = t->texture;
	if (h.kind != core::res::TextureKind::Volume || h.format != core::res::TextureFormat::Argb8888 || h.width != kGradeLutSide ||
			h.height != kGradeLutSide || h.depth != kGradeLutSide) {
		r_note = p_name + ": not a " + std::to_string(kGradeLutSide) + "^3 ARGB8888 volume";
		return {};
	}
	std::vector<core::res::MipSlice> mips;
	if (const core::res::ResError e = t->mips(mips); !e.ok() || mips.empty()) {
		r_note = p_name + ": " + (e.ok() ? std::string("no mips") : e.message());
		return {};
	}
	const core::res::MipSlice &m = mips.front();
	const size_t row = size_t(kGradeLutSide) * 4;
	const size_t pitch = std::max<size_t>(m.row_pitch, row);
	const size_t rows = size_t(kGradeLutSide) * kGradeLutSide;
	if (m.data.size() < pitch * (rows - 1) + row) {
		r_note = p_name + ": volume data short";
		return {};
	}
	std::vector<uint8_t> out(rows * row);
	for (size_t r = 0; r < rows; ++r) {
		std::copy_n(m.data.data() + r * pitch, row, out.data() + r * row);
	}
	return out;
}

bool point_in_polygon(const std::vector<std::array<float, 2>> &p_outline, float p_x, float p_z) {
	bool inside = false;
	for (size_t i = 0, j = p_outline.size() - 1; i < p_outline.size(); j = i++) {
		const auto &a = p_outline[i], &b = p_outline[j];
		if ((a[1] > p_z) != (b[1] > p_z)) {
			const float x = (b[0] - a[0]) * (p_z - a[1]) / (b[1] - a[1]) + a[0];
			if (p_x < x) {
				inside = !inside;
			}
		}
	}
	return inside;
}

float segment_distance(const std::array<float, 2> &p_a, const std::array<float, 2> &p_b, float p_x, float p_z) {
	const float dx = p_b[0] - p_a[0], dz = p_b[1] - p_a[1];
	const float len2 = dx * dx + dz * dz;
	float t = len2 > 0.0f ? ((p_x - p_a[0]) * dx + (p_z - p_a[1]) * dz) / len2 : 0.0f;
	t = std::clamp(t, 0.0f, 1.0f);
	const float ex = p_a[0] + dx * t - p_x, ez = p_a[1] + dz * t - p_z;
	return std::sqrt(ex * ex + ez * ez);
}

void blend_state(LookEnvState &r_state, const LookEnvState &p_a, float p_w) {
	const bool take = p_w >= 0.5f;
	if (p_a.light) {
		r_state.light = true;
		r_state.sun_color = lerp3(r_state.sun_color, p_a.sun_color, p_w);
		r_state.sun_rotation_x = lerp1(r_state.sun_rotation_x, p_a.sun_rotation_x, p_w);
		r_state.sun_rotation_y = lerp1(r_state.sun_rotation_y, p_a.sun_rotation_y, p_w);
		r_state.outdoor_sky = lerp3(r_state.outdoor_sky, p_a.outdoor_sky, p_w);
		r_state.outdoor_ground = lerp3(r_state.outdoor_ground, p_a.outdoor_ground, p_w);
	}
	if (p_a.enlighten) {
		r_state.enlighten = true;
		r_state.sky_color = lerp3(r_state.sky_color, p_a.sky_color, p_w);
		r_state.ground_color = lerp3(r_state.ground_color, p_a.ground_color, p_w);
		r_state.bounce_scale = lerp1(r_state.bounce_scale, p_a.bounce_scale, p_w);
		r_state.enlighten_sun_scale = lerp1(r_state.enlighten_sun_scale, p_a.enlighten_sun_scale, p_w);
	}
	if (p_a.sky) {
		r_state.sky = true;
		if (take) {
			r_state.panorama = p_a.panorama;
			r_state.panorama_alpha = p_a.panorama_alpha;
			r_state.gradient = p_a.gradient;
			r_state.uv_min_y = p_a.uv_min_y;
			r_state.uv_max_y = p_a.uv_max_y;
			r_state.sky_rotation = p_a.sky_rotation;
		}
		r_state.sky_brightness = lerp1(r_state.sky_brightness, p_a.sky_brightness, p_w);
		r_state.horizon = lerp3(r_state.horizon, p_a.horizon, p_w);
	}
	if (p_a.fog) {
		if (!r_state.fog) {

			r_state.fog_start = p_a.fog_start;
			r_state.fog_end = p_a.fog_end;
			r_state.fog_multiplier = p_a.fog_multiplier;
			r_state.fog_peak = 0.0f;
			r_state.fog_exponent = p_a.fog_exponent;
			r_state.fog_fit_end = p_a.fog_fit_end;
			r_state.fog_color = p_a.fog_color;
		}
		r_state.fog = true;
		r_state.fog_start = lerp1(r_state.fog_start, p_a.fog_start, p_w);
		r_state.fog_end = lerp1(r_state.fog_end, p_a.fog_end, p_w);
		r_state.fog_multiplier = lerp1(r_state.fog_multiplier, p_a.fog_multiplier, p_w);
		r_state.fog_peak = lerp1(r_state.fog_peak, p_a.fog_peak, p_w);
		r_state.fog_exponent = lerp1(r_state.fog_exponent, p_a.fog_exponent, p_w);
		r_state.fog_fit_end = lerp1(r_state.fog_fit_end, p_a.fog_fit_end, p_w);
		r_state.fog_color = lerp3(r_state.fog_color, p_a.fog_color, p_w);
		r_state.fog_tint = lerp1(r_state.fog_tint, p_a.fog_tint, p_w);
	}
	if (p_a.tonemap) {
		r_state.tonemap = true;
		r_state.middle_gray = lerp1(r_state.middle_gray, p_a.middle_gray, p_w);
		r_state.min_exposure = lerp1(r_state.min_exposure, p_a.min_exposure, p_w);
		r_state.max_exposure = lerp1(r_state.max_exposure, p_a.max_exposure, p_w);
		r_state.exposure_time = lerp1(r_state.exposure_time, p_a.exposure_time, p_w);
		r_state.bloom = lerp3(r_state.bloom, p_a.bloom, p_w);
	}
	if (p_a.correction && take) {
		r_state.correction = true;
		r_state.lut = p_a.lut;
	}
}

std::string lut_key(const LookEnvState &p_s) {
	char buf[256];
	std::snprintf(buf, sizeof(buf), "%a %a %a|%a %a %a|%a %a %a|", double(p_s.brightness[0]), double(p_s.brightness[1]), double(p_s.brightness[2]),
			double(p_s.contrast[0]), double(p_s.contrast[1]), double(p_s.contrast[2]), double(p_s.saturation[0]), double(p_s.saturation[1]),
			double(p_s.saturation[2]));
	return std::string(buf) + p_s.grade_cube;
}

}

float look_cubic(const LookCubic &p_c, float p_t) {
	return ((p_c[0] * p_t + p_c[1]) * p_t + p_c[2]) * p_t + p_c[3];
}

void fog_fit(const LookCubic &p_curve, float p_reach, float &r_density, float &r_curve, float &r_end) {
	r_density = 0.0f;
	r_curve = 1.0f;
	r_end = 1.0f;
	const float reach = std::isfinite(p_reach) ? std::clamp(p_reach, 1.0f / 64.0f, 1.0f) : 1.0f;
	float want[kFogFitSamples + 1];
	float top = 0.0f;
	for (uint32_t i = 0; i <= kFogFitSamples; ++i) {
		const float t = reach * float(i) / float(kFogFitSamples);
		const float v = look_cubic(p_curve, t);
		want[i] = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
		top = std::fmax(top, want[i]);
	}
	if (!(top > 0.0f)) {
		return;
	}
	double best = INFINITY;
	for (uint32_t e = 0; e < kFogFitSteps / 2; ++e) {

		const float end = 1.0f / 64.0f + (1.0f - 1.0f / 64.0f) * float(e) / float(kFogFitSteps / 2 - 1);
		const float at_end = look_cubic(p_curve, end);
		const float density = std::isfinite(at_end) ? std::clamp(at_end, 0.0f, 1.0f) : 0.0f;
		if (!(density > 0.0f)) {
			continue;
		}
		for (uint32_t s = 0; s <= kFogFitSteps / 2; ++s) {
			const float k = 0.05f * std::pow(320.0f, float(s) / float(kFogFitSteps / 2));
			double error = 0.0;
			for (uint32_t i = 0; i <= kFogFitSamples; ++i) {
				const float t = std::clamp(reach * float(i) / float(kFogFitSamples) / end, 0.0f, 1.0f);
				const double ease = double(t) * t * (3.0 - 2.0 * t);
				const double got = double(density) * std::pow(ease, double(k));
				error += (got - want[i]) * (got - want[i]);
			}
			if (error < best) {
				best = error;
				r_density = density;
				r_curve = k;
				r_end = end;
			}
		}
	}
}

LookVec3 look_grade(const LookEnvState &p_state, const std::vector<uint8_t> &p_cube, const LookVec3 &p_display) {
	LookVec3 c{};
	for (size_t k = 0; k < 3; ++k) {
		c[k] = std::clamp(((p_display[k] - 0.5f) * p_state.contrast[k] + 0.5f) * p_state.brightness[k], 0.0f, 1.0f);
	}
	const float luma = c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
	for (size_t k = 0; k < 3; ++k) {
		c[k] = std::clamp(luma + (c[k] - luma) * p_state.saturation[k], 0.0f, 1.0f);
	}
	const size_t n = kGradeLutSide;
	if (p_cube.size() != n * n * n * 4) {
		return c;
	}
	size_t lo[3], hi[3];
	float f[3];
	for (size_t a = 0; a < 3; ++a) {
		const float at = c[a] * float(n - 1);
		lo[a] = std::min(size_t(at), n - 1);
		hi[a] = std::min(lo[a] + 1, n - 1);
		f[a] = at - float(lo[a]);
	}
	LookVec3 out{};
	for (int corner = 0; corner < 8; ++corner) {
		const size_t r = (corner & 1) ? hi[0] : lo[0], g = (corner & 2) ? hi[1] : lo[1], b = (corner & 4) ? hi[2] : lo[2];
		const float w = ((corner & 1) ? f[0] : 1.0f - f[0]) * ((corner & 2) ? f[1] : 1.0f - f[1]) * ((corner & 4) ? f[2] : 1.0f - f[2]);
		const size_t at = ((b * n + g) * n + r) * 4;
		for (size_t k = 0; k < 3; ++k) {
			out[k] += w * float(p_cube[at + k]) / 255.0f;
		}
	}
	return out;
}

core::Result<LookEnvironment> read_look_environment(const EbxResolver &p_db, const World &p_world) {
	LookEnvironment env;
	const int32_t visibility_field = core::level::field_id("Visibility");
	std::vector<Ve> ves;
	for (const PlacedObject &object : p_world.objects) {
		if (object.kind != ObjectKind::VisualEnvironment || !object.reference || !object.entity.is_a("VisualEnvironmentEntityData")) {
			continue;
		}
		if (ves.size() >= kMaxVisualEnvironments) {
			env.notes.push_back("more than " + std::to_string(kMaxVisualEnvironments) + " visual environments: the rest ignored");
			break;
		}
		Ve ve;
		ve.object = &object;
		ve.reference = *object.reference;
		const std::optional<EbxValue> priority = ve.reference.get("Priority");
		ve.priority = priority ? priority->as_i64().value_or(0) : 0;
		ve.name = partition_name(p_db, object.entity);
		const core::level::Connections &connections = p_world.scopes.connections(object.scope);
		for (const core::level::Connection &c : connections.property) {
			if (!(c.target == ve.reference) && !(c.target == object.entity)) {
				continue;
			}
			if (c.target == ve.reference && c.target_field == visibility_field && c.source.is_a("AreaProximityEntityData")) {
				ve.proximity = c.source;
			} else {
				ve.switched = true;
			}
		}
		for (const core::level::Connection &c : connections.event) {
			if (c.target == ve.reference || c.target == object.entity) {
				ve.switched = true;
			}
		}
		ves.push_back(std::move(ve));
	}
	if (ves.empty()) {
		env.notes.push_back("no placed visual environment");
		return env;
	}

	const char *kinds[] = { "OutdoorLightComponentData", "EnlightenComponentData", "SkyComponentData", "FogComponentData", "TonemapComponentData",
		"ColorCorrectionComponentData" };
	for (const char *kind : kinds) {
		const Ve *best = nullptr;
		std::optional<EbxInstance> best_component;
		for (const Ve &ve : ves) {
			if (ve.proximity) {
				continue;
			}
			if (const std::optional<EbxValue> v = ve.object->entity.get("Visibility"); v && v->as_f32() && *v->as_f32() <= 0.0f) {
				continue;
			}
			std::optional<EbxInstance> found;
			size_t seen = 0;
			for (const EbxInstance &c : p_db.follow_all(ve.object->entity.data(), "Components")) {
				if (++seen > kMaxComponents) {
					break;
				}
				if (c.is_a(kind) && flag(c, "Enable", true)) {
					found = c;
					break;
				}
			}
			if (!found) {
				continue;
			}
			bool better = best == nullptr;
			if (!better) {
				if (best->switched != ve.switched) {
					better = !ve.switched;
				} else {
					better = ve.switched ? ve.priority < best->priority : ve.priority > best->priority;
				}
			}
			if (better) {
				best = &ve;
				best_component = found;
			}
		}
		if (best != nullptr && best_component) {
			LookEnvState probe;
			if (read_component(p_db, *best_component, best->name, probe)) {
				blend_state(env.base, probe, 1.0f);
				env.base.source += (env.base.source.empty() ? "" : " ") + probe.source;
				if (probe.correction) {
					env.base.brightness = probe.brightness;
					env.base.contrast = probe.contrast;
					env.base.saturation = probe.saturation;
					env.base.grade_cube = probe.grade_cube;
				}
				if (probe.sky) {
					env.base.panorama = probe.panorama;
					env.base.panorama_alpha = probe.panorama_alpha;
					env.base.gradient = probe.gradient;
					env.base.sky_rotation = probe.sky_rotation;
					env.base.uv_min_y = probe.uv_min_y;
					env.base.uv_max_y = probe.uv_max_y;
				}
				if (probe.fog) {
					env.base.fog_color_enable = probe.fog_color_enable;
					env.base.fog_curve = probe.fog_curve;
					env.base.fog_color_curve = probe.fog_color_curve;
				}
				env.from_data = true;
			}
		}
	}

	for (const Ve &ve : ves) {
		if (!ve.proximity) {
			continue;
		}
		std::optional<LookArea> area = area_shape(p_world, ve.object->scope, *ve.proximity);
		if (!area) {
			env.notes.push_back(ve.name + ": area without a polygon or box shape, ignored");
			continue;
		}
		area->name = ve.name;
		area->priority = ve.priority;
		size_t seen = 0, read = 0;
		for (const EbxInstance &c : p_db.follow_all(ve.object->entity.data(), "Components")) {
			if (++seen > kMaxComponents) {
				break;
			}
			read += read_component(p_db, c, ve.name, area->state) ? 1 : 0;
		}
		if (read == 0) {
			env.notes.push_back(ve.name + ": area with no component the look reads, ignored");
			continue;
		}
		env.areas.push_back(std::move(*area));
	}
	std::stable_sort(env.areas.begin(), env.areas.end(), [](const LookArea &a, const LookArea &b) { return a.priority < b.priority; });
	return env;
}

void decode_look_textures(const core::LevelMount &p_mount, LookEnvironment &r_env) {
	std::map<std::string, std::optional<LookVec3>> horizons;
	std::map<std::string, int32_t> luts;
	std::map<std::string, std::vector<uint8_t>> cubes;
	const auto decode = [&](LookEnvState &r_state) {
		if (r_state.sky && !r_state.panorama.empty()) {
			auto found = horizons.find(r_state.panorama);
			if (found == horizons.end()) {
				std::string note;
				found = horizons.emplace(r_state.panorama, horizon_of(p_mount, r_state.panorama, note)).first;
				if (!note.empty()) {
					r_env.notes.push_back("horizon " + note);
				}
			}
			if (found->second) {
				r_state.horizon = *found->second;
				r_state.horizon_decoded = true;
			}
		}
		if (!r_state.correction) {
			return;
		}
		const std::string key = lut_key(r_state);
		if (const auto known = luts.find(key); known != luts.end()) {
			r_state.lut = known->second;
			return;
		}
		auto cube = cubes.find(r_state.grade_cube);
		if (cube == cubes.end()) {
			std::string note;
			std::vector<uint8_t> bytes = r_state.grade_cube.empty() ? std::vector<uint8_t>() : grade_cube_of(p_mount, r_state.grade_cube, note);
			if (!note.empty()) {
				r_env.notes.push_back("grading " + note);
			}
			cube = cubes.emplace(r_state.grade_cube, std::move(bytes)).first;
		}
		const size_t n = kGradeLutSide;
		std::vector<uint8_t> lut(n * n * n * 4);
		for (size_t b = 0; b < n; ++b) {
			for (size_t g = 0; g < n; ++g) {
				for (size_t r = 0; r < n; ++r) {

					const LookVec3 in = { (float(r) + 0.5f) / float(n), (float(g) + 0.5f) / float(n), (float(b) + 0.5f) / float(n) };
					const LookVec3 out = look_grade(r_state, cube->second, in);
					const size_t at = ((b * n + g) * n + r) * 4;
					for (size_t k = 0; k < 3; ++k) {
						lut[at + k] = uint8_t(std::lround(std::clamp(out[k], 0.0f, 1.0f) * 255.0f));
					}
					lut[at + 3] = 255;
				}
			}
		}
		r_state.lut = int32_t(r_env.luts.size());
		r_env.luts.push_back(std::move(lut));
		luts.emplace(key, r_state.lut);
	};
	decode(r_env.base);
	for (LookArea &a : r_env.areas) {
		decode(a.state);
	}
}

float look_area_weight(const LookArea &p_area, const LookVec3 &p_position) {
	if (!finite3(p_position)) {
		return 0.0f;
	}
	float distance = 0.0f;
	if (p_area.box) {
		const LookVec3 d = { p_position[0] - p_area.box_center[0], p_position[1] - p_area.box_center[1], p_position[2] - p_area.box_center[2] };
		float sum = 0.0f;
		for (size_t a = 0; a < 3; ++a) {
			const LookVec3 &axis = p_area.box_axes[a];
			const float local = d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2];
			const float out = std::fmax(std::fabs(local) - p_area.box_half[a], 0.0f);
			sum += out * out;
		}
		distance = std::sqrt(sum);
	} else {
		if (p_area.outline.size() < 3) {
			return 0.0f;
		}
		float horizontal = 0.0f;
		if (!point_in_polygon(p_area.outline, p_position[0], p_position[2])) {
			horizontal = INFINITY;
			for (size_t i = 0, j = p_area.outline.size() - 1; i < p_area.outline.size(); j = i++) {
				horizontal = std::fmin(horizontal, segment_distance(p_area.outline[j], p_area.outline[i], p_position[0], p_position[2]));
			}
		}
		const float vertical = std::fmax(std::fmax(p_area.floor - p_position[1], p_position[1] - (p_area.floor + p_area.height)), 0.0f);
		distance = std::sqrt(horizontal * horizontal + vertical * vertical);
	}
	if (!(distance > 0.0f)) {
		return std::isfinite(distance) ? 1.0f : 0.0f;
	}
	if (!(p_area.fade > 0.0f) || !std::isfinite(distance)) {
		return 0.0f;
	}
	return std::fmax(0.0f, 1.0f - distance / p_area.fade);
}

LookFrame look_frame_of(const LookEnvState &p_s) {
	LookFrame f;
	const float az = p_s.sun_rotation_x * kPi / 180.0f, el = p_s.sun_rotation_y * kPi / 180.0f;
	const LookVec3 to_sun = { std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el) };
	f.sun_direction = { -to_sun[0], -to_sun[1], -to_sun[2] };
	const float peak = max3(p_s.sun_color);
	if (peak > 1e-6f) {
		f.sun_color = scaled(p_s.sun_color, 1.0f / peak);
		f.sun_energy = peak;
	} else {
		f.sun_color = { 1.0f, 1.0f, 1.0f };
		f.sun_energy = 0.0f;
	}
	const LookVec3 sky = p_s.enlighten && max3(p_s.sky_color) > 1e-6f ? p_s.sky_color : p_s.outdoor_sky;
	const LookVec3 ground = p_s.enlighten && max3(p_s.ground_color) > 1e-6f ? p_s.ground_color : p_s.outdoor_ground;
	const LookVec3 bounce = p_s.enlighten
			? scaled(p_s.sun_color, std::fmax(std::sin(el), 0.0f) * kBounceAlbedo * std::fmax(p_s.bounce_scale, 0.0f))
			: LookVec3{};
	f.ambient_up = added(sky, scaled(bounce, 0.25f));
	f.ambient_horizon = added(scaled(added(sky, ground), 0.5f), scaled(bounce, 0.5f));
	f.ambient_down = added(ground, bounce);
	f.sky_brightness = p_s.sky_brightness;
	f.sky_rotation = p_s.sky_rotation;
	f.uv_min_y = p_s.uv_min_y;
	f.uv_max_y = p_s.uv_max_y;
	f.horizon = scaled(p_s.horizon, p_s.sky_brightness);
	if (p_s.fog && p_s.fog_peak > 0.0f && p_s.fog_end > p_s.fog_start) {
		f.fog = true;
		f.fog_begin = p_s.fog_start / p_s.fog_multiplier;
		f.fog_end = (p_s.fog_start + p_s.fog_fit_end * (p_s.fog_end - p_s.fog_start)) / p_s.fog_multiplier;
		f.fog_density = p_s.fog_peak;
		f.fog_curve = p_s.fog_exponent;
		const float level = max3(f.horizon);
		const float tint_peak = max3(p_s.fog_color);
		const LookVec3 tint = tint_peak > 1e-6f ? scaled(p_s.fog_color, level / tint_peak) : f.horizon;
		f.fog_color = lerp3(f.horizon, tint, p_s.fog_tint);
	}
	f.middle_gray = p_s.middle_gray;
	f.min_exposure = p_s.min_exposure;
	f.max_exposure = p_s.max_exposure;
	f.exposure_speed = 1.0f / std::fmax(p_s.exposure_time, 0.01f);

	const auto luma = [](const LookVec3 &p_c) { return 0.2126f * p_c[0] + 0.7152f * p_c[1] + 0.0722f * p_c[2]; };
	const float ambient = 0.5f * (luma(f.ambient_up) + luma(f.ambient_horizon));
	const float average = kReferenceAlbedo * (ambient + kSunlitShare * f.sun_energy * luma(f.sun_color) * std::fmax(std::sin(el), 0.0f));
	f.exposure = average > 1e-6f ? std::clamp(f.middle_gray / average, f.min_exposure, f.max_exposure) : f.max_exposure;
	f.bloom = std::fmax(max3(p_s.bloom), 0.0f);
	f.lut = p_s.lut;
	return f;
}

LookFrame look_frame(const LookEnvironment &p_env, const LookVec3 &p_position) {
	LookEnvState s = p_env.base;
	for (const LookArea &a : p_env.areas) {
		const float w = look_area_weight(a, p_position);
		if (w > 0.0f) {
			blend_state(s, a.state, w);
		}
	}
	return look_frame_of(s);
}

}
