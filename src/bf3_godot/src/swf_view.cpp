#include "swf_view.h"

#include "godot_util.h"
#include "ui_assets.h"
#include "ui_stage.h"
#include "ui_textures.h"

#include "bf3/core/level/budget.h"
#include "bf3/core/ui/swf_font.h"
#include "bf3/core/ui/swf_geometry.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/fnv.h"

#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/transform2d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;
using bf3::core::make_error;
using bf3::core::Status;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kGlyphFlattenUnits = ui::kSwfFlattenTolerance * float(ui::kSwfFontUnitsPerEm) / 128.0f;
constexpr double kRoundTolerance = 0.05;
constexpr size_t kGeometryBudget = size_t(1) << 22;
constexpr int32_t kKernelSlots = 64;

constexpr int32_t kModeSolid = 0;
constexpr int32_t kModeBitmap = 1;
constexpr int32_t kModeClipped = 2;
constexpr int32_t kModeLayer = 3;
constexpr int32_t kFlagWrap = 1;
constexpr int32_t kFlagAdditive = 2;

const char *kItemShader = R"(shader_type canvas_item;
render_mode skip_vertex_transform, blend_premul_alpha;
uniform sampler2D mask_tex : hint_default_white, filter_nearest;
uniform int coverage = 0;
instance uniform vec3 h0 = vec3(1.0, 0.0, 0.0);
instance uniform vec3 h1 = vec3(0.0, 1.0, 0.0);
instance uniform vec3 h2 = vec3(0.0, 0.0, 1.0);
instance uniform vec4 cx_mul = vec4(1.0);
instance uniform vec4 cx_add = vec4(0.0);
instance uniform vec4 uv_rect = vec4(0.0);
instance uniform vec2 atlas_inv = vec2(1.0);
instance uniform int mode = 0;
instance uniform int flags = 0;
varying vec3 uvw;
varying vec4 tint;
void vertex() {
	vec2 p = (MODEL_MATRIX * vec4(VERTEX, 0.0, 1.0)).xy;
	vec3 hp = vec3(p, 1.0);
	vec3 q = vec3(dot(h0, hp), dot(h1, hp), dot(h2, hp));
	float w = 1.0 / q.z;
	VERTEX = q.xy * w;
	uvw = vec3(UV * w, w);
	tint = COLOR;
}
void fragment() {
	vec2 t = uvw.xy / uvw.z;
	vec4 c = tint;
	vec4 hi = vec4(1.0);
	bool raw = false;
	if (mode == 1) {
		vec2 s = uv_rect.zw;
		vec2 k = (flags & 1) != 0 ? mod(t, s) : clamp(t, vec2(0.5), max(s - vec2(0.5), vec2(0.5)));
		c *= texture(TEXTURE, (uv_rect.xy + k) * atlas_inv);
	} else if (mode == 2) {
		if (t.x < uv_rect.x || t.y < uv_rect.y || t.x > uv_rect.z || t.y > uv_rect.w) {
			discard;
		}
	} else if (mode == 3) {
		vec4 p = texture(TEXTURE, t) * texture(mask_tex, t).a;
		if (p.a <= 0.0) {
			if (max(p.r, max(p.g, p.b)) <= 0.0) {
				discard;
			}
			raw = true;
			c = vec4(p.rgb * cx_mul.rgb * cx_mul.a, 0.0);
		} else {
			c = vec4(p.rgb / p.a, p.a);
			hi.rgb = max(vec3(1.0), c.rgb);
		}
	}
	if (raw) {
		COLOR = c;
	} else {
		c = clamp(c * cx_mul + cx_add, vec4(0.0), hi);
		COLOR = vec4(c.rgb * c.a, (flags & 2) != 0 && coverage == 0 ? 0.0 : c.a);
	}
}
)";

const char *kFilterShader = R"(shader_type canvas_item;
render_mode blend_premul_alpha;
uniform vec2 shift = vec2(0.0);
uniform int taps_x = 1;
uniform int taps_y = 1;
uniform float weights_x[64];
uniform float weights_y[64];
uniform vec4 shadow = vec4(0.0);
uniform float strength = 1.0;
uniform int inner = 0;
uniform int knockout = 0;
uniform int source = 1;
uniform int blur = 0;
void fragment() {
	float hx = float(taps_x - 1) * 0.5;
	float hy = float(taps_y - 1) * 0.5;
	if (blur == 1) {
		vec4 acc = vec4(0.0);
		for (int j = 0; j < taps_y; j++) {
			vec4 row = vec4(0.0);
			for (int i = 0; i < taps_x; i++) {
				row += weights_x[i] * texture(TEXTURE, UV + vec2(float(i) - hx, float(j) - hy) * TEXTURE_PIXEL_SIZE);
			}
			acc += weights_y[j] * row;
		}
		COLOR = acc;
	} else {
	float a = 0.0;
	for (int j = 0; j < taps_y; j++) {
		float row = 0.0;
		for (int i = 0; i < taps_x; i++) {
			vec2 o = (vec2(float(i) - hx, float(j) - hy) - shift) * TEXTURE_PIXEL_SIZE;
			float s = texture(TEXTURE, UV + o).a;
			row += weights_x[i] * (inner == 1 ? 1.0 - s : s);
		}
		a += weights_y[j] * row;
	}
	a = clamp(a * strength, 0.0, 1.0) * shadow.a;
	vec4 src = texture(TEXTURE, UV);
	vec4 outc;
	if (inner == 1) {
		outc = knockout == 1 ? vec4(shadow.rgb * a * src.a, a * src.a) : vec4(src.rgb * (1.0 - a) + shadow.rgb * a * src.a, src.a);
	} else {
		vec4 sh = vec4(shadow.rgb * a, a);
		outc = knockout == 1 ? sh * (1.0 - src.a) : (source == 1 ? src + sh * (1.0 - src.a) : sh);
	}
	COLOR = outc;
	}
}
)";

const char *kPresentShader = R"(shader_type canvas_item;
render_mode blend_premul_alpha;
void fragment() {
	COLOR = texture(TEXTURE, UV);
}
)";

struct SharedShaders {
	RID item;
	RID filter;
	RID present;
	uint32_t users = 0;
};

std::unique_ptr<SharedShaders> g_shaders;
uint64_t g_open_serial = 0;
double g_sync_seconds = 0.0;
SwfViewCounters g_counters;
std::vector<const Bf3SwfView *> g_views;

const SharedShaders &acquire_shaders(RenderingServer *p_rs) {
	if (g_shaders == nullptr) {
		g_shaders = std::make_unique<SharedShaders>();
		g_shaders->item = p_rs->shader_create();
		p_rs->shader_set_code(g_shaders->item, kItemShader);
		g_shaders->filter = p_rs->shader_create();
		p_rs->shader_set_code(g_shaders->filter, kFilterShader);
		g_shaders->present = p_rs->shader_create();
		p_rs->shader_set_code(g_shaders->present, kPresentShader);
	}
	++g_shaders->users;
	return *g_shaders;
}

void release_shaders(RenderingServer *p_rs) {
	if (g_shaders == nullptr || --g_shaders->users != 0) {
		return;
	}
	for (const RID &r : { g_shaders->item, g_shaders->filter, g_shaders->present }) {
		p_rs->free_rid(r);
	}
	g_shaders.reset();
}

constexpr ui::SwfAffine kSingularAffine{ 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

template <class T> void fnv1a_items(uint64_t &r_hash, const std::vector<T> &p_items) {
	bf3::core::fnv1a_value(r_hash, p_items.size());
	bf3::core::fnv1a(r_hash, p_items.data(), p_items.size() * sizeof(T));
}

void fnv1a_fill(uint64_t &r_hash, const ui::SwfFillStyle &p_f) {
	bf3::core::fnv1a_value(r_hash, p_f.type);
	bf3::core::fnv1a_value(r_hash, p_f.kind);
	bf3::core::fnv1a_value(r_hash, p_f.color);
	bf3::core::fnv1a_value(r_hash, p_f.bitmap_id);
	bf3::core::fnv1a_value(r_hash, p_f.matrix);
	bf3::core::fnv1a_value(r_hash, p_f.spread);
	bf3::core::fnv1a_value(r_hash, p_f.interpolation);
	bf3::core::fnv1a_value(r_hash, p_f.focal);
	fnv1a_items(r_hash, p_f.stops);
}

void fnv1a_drawing(uint64_t &r_hash, const ui::SwfShape &p_s) {
	bf3::core::fnv1a_value(r_hash, p_s.bounds);
	for (const ui::SwfFillStyle &f : p_s.fills) {
		fnv1a_fill(r_hash, f);
	}
	for (const ui::SwfLineStyle &l : p_s.lines) {
		bf3::core::fnv1a_value(r_hash, l.width);
		bf3::core::fnv1a_value(r_hash, l.color);
		for (const bool b : { l.no_hscale, l.no_vscale, l.pixel_hinting, l.no_close, l.has_fill }) {
			bf3::core::fnv1a_value(r_hash, b);
		}
		bf3::core::fnv1a_value(r_hash, l.start_cap);
		bf3::core::fnv1a_value(r_hash, l.end_cap);
		bf3::core::fnv1a_value(r_hash, l.join);
		bf3::core::fnv1a_value(r_hash, l.miter_limit);
		fnv1a_fill(r_hash, l.fill);
	}
	for (const ui::SwfFillMesh &m : p_s.meshes) {
		bf3::core::fnv1a_value(r_hash, m.fill);
		bf3::core::fnv1a_value(r_hash, m.open_contours);
		fnv1a_items(r_hash, m.outline);
		fnv1a_items(r_hash, m.contour_ends);
		fnv1a_items(r_hash, m.vertices);
		fnv1a_items(r_hash, m.indices);
	}
	for (const ui::SwfStroke &st : p_s.strokes) {
		bf3::core::fnv1a_value(r_hash, st.line);
		fnv1a_items(r_hash, st.points);
		fnv1a_items(r_hash, st.path_ends);
		for (const bool c : st.closed) {
			bf3::core::fnv1a_value(r_hash, c);
		}
	}
}

uint64_t content_fingerprint(const ui::SwfSceneNode &p_n) {
	uint64_t h = bf3::core::kFnv1aOffset;
	bf3::core::fnv1a_value(h, p_n.kind);
	for (const void *p : { static_cast<const void *>(p_n.shape), static_cast<const void *>(p_n.edit_text),
				 static_cast<const void *>(p_n.sub_image) }) {
		bf3::core::fnv1a_value(h, reinterpret_cast<uintptr_t>(p));
	}
	bf3::core::fnv1a_value(h, p_n.drawing != nullptr);
	if (p_n.drawing != nullptr) {
		fnv1a_drawing(h, *p_n.drawing);
	}
	bf3::core::fnv1a(h, p_n.image.data(), p_n.image.size());
	bf3::core::fnv1a_value(h, p_n.image_width);
	bf3::core::fnv1a_value(h, p_n.image_height);
	bf3::core::fnv1a(h, p_n.text.data(), p_n.text.size());
	bf3::core::fnv1a_value(h, p_n.html);
	bf3::core::fnv1a_value(h, p_n.text_options.auto_size);
	bf3::core::fnv1a_value(h, p_n.text_options.fit);
	bf3::core::fnv1a_value(h, p_n.text_bounds.has_value());
	if (p_n.text_bounds) {
		bf3::core::fnv1a_value(h, *p_n.text_bounds);
	}
	bf3::core::fnv1a_value(h, p_n.text_color.has_value());
	if (p_n.text_color) {
		bf3::core::fnv1a_value(h, *p_n.text_color);
	}
	bf3::core::fnv1a_value(h, p_n.text_size.value_or(-1.0f));
	bf3::core::fnv1a_value(h, int32_t(p_n.text_align.value_or(0xFF)));
	return h;
}

Transform2D xform_of(const ui::SwfAffine &p_m) {
	return Transform2D(real_t(p_m.a), real_t(p_m.b), real_t(p_m.c), real_t(p_m.d), real_t(p_m.tx), real_t(p_m.ty));
}

struct Hom {
	std::array<double, 9> m{ 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
	bool identity = true;
};

Hom hmul(const Hom &p_l, const Hom &p_r) {
	if (p_l.identity) {
		return p_r;
	}
	if (p_r.identity) {
		return p_l;
	}
	Hom h;
	h.identity = false;
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			h.m[i * 3 + j] = p_l.m[i * 3 + 0] * p_r.m[0 * 3 + j] + p_l.m[i * 3 + 1] * p_r.m[1 * 3 + j] + p_l.m[i * 3 + 2] * p_r.m[2 * 3 + j];
		}
	}
	return h;
}

Hom hom_of(const ui::SwfAffine &p_m) {
	Hom h;
	h.identity = false;
	h.m = { p_m.a, p_m.c, p_m.tx, p_m.b, p_m.d, p_m.ty, 0.0, 0.0, 1.0 };
	return h;
}

ui::SwfPoint happly(const Hom &p_h, double p_x, double p_y) {
	if (p_h.identity) {
		return { float(p_x), float(p_y) };
	}
	const double x = p_h.m[0] * p_x + p_h.m[1] * p_y + p_h.m[2];
	const double y = p_h.m[3] * p_x + p_h.m[4] * p_y + p_h.m[5];
	double w = p_h.m[6] * p_x + p_h.m[7] * p_y + p_h.m[8];
	w = std::abs(w) < 1e-9 ? 1e-9 : w;
	return { float(x / w), float(y / w) };
}

Hom tilt_homography(const SwfTilt &p_tilt, double p_px, double p_py, const ui::SwfRect &p_stage) {
	const double cx = (p_stage.x_min + p_stage.x_max) * 0.5;
	const double cy = (p_stage.y_min + p_stage.y_max) * 0.5;
	const double fov = std::clamp(double(p_tilt.perspective_fov), 1.0, 179.0) * bf3::core::kDegToRad;
	const double f = (p_stage.height() * 0.5) / std::tan(fov * 0.5);
	const double ax = double(p_tilt.x_rotation) * bf3::core::kDegToRad;
	const double ay = double(p_tilt.y_rotation) * bf3::core::kDegToRad;
	const double sx = std::sin(ax);
	const double cxr = std::cos(ax);
	const double sy = std::sin(ay);
	const double cyr = std::cos(ay);
	const std::array<double, 9> rx{ 1.0, 0.0, 0.0, 0.0, cxr, -sx, 0.0, sx, cxr };
	const std::array<double, 9> ry{ cyr, 0.0, sy, 0.0, 1.0, 0.0, -sy, 0.0, cyr };
	std::array<double, 9> r{};
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			r[i * 3 + j] = ry[i * 3 + 0] * rx[0 * 3 + j] + ry[i * 3 + 1] * rx[1 * 3 + j] + ry[i * 3 + 2] * rx[2 * 3 + j];
		}
	}
	Hom p;
	p.identity = false;
	p.m = { cx * r[6] + f * r[0], cx * r[7] + f * r[1], cx * f + f * (p_px - cx), cy * r[6] + f * r[3], cy * r[7] + f * r[4], cy * f + f * (p_py - cy), r[6], r[7],
		f };
	Hom t;
	t.identity = false;
	t.m = { 1.0, 0.0, -p_px, 0.0, 1.0, -p_py, 0.0, 0.0, 1.0 };
	return hmul(p, t);
}

constexpr int32_t kLayerGranule = 32;

int32_t layer_alloc(int32_t p_need) {
	const int32_t grown = p_need + p_need / 8;
	return std::min(Bf3SwfView::kMaxLayerSide, std::max(p_need, (grown + kLayerGranule - 1) / kLayerGranule * kLayerGranule));
}

bool layer_fits(Vector2i p_have, Vector2i p_need) {
	return p_need.x <= p_have.x && p_need.y <= p_have.y &&
			(p_have.x <= kLayerGranule * 2 || p_need.x * 4 >= p_have.x) &&
			(p_have.y <= kLayerGranule * 2 || p_need.y * 4 >= p_have.y);
}

Vector4 vec4_of(const Color &p_c) {
	return Vector4(p_c.r, p_c.g, p_c.b, p_c.a);
}

ui::SwfBounds project_rect(const ui::SwfRect &p_r, const ui::SwfAffine &p_m, const Hom &p_h) {
	return ui::swf_map_rect(p_r, [&](double p_x, double p_y) {
		const ui::SwfPoint q = ui::swf_apply(p_m, p_x, p_y);
		return happly(p_h, q.x, q.y);
	});
}

Color color_of(ui::SwfColor p_c) {
	return Color(float(p_c.r) / 255.0f, float(p_c.g) / 255.0f, float(p_c.b) / 255.0f, float(p_c.a) / 255.0f);
}

using FilterMaterialKey = std::array<float, 14>;

struct FilterMaterial {
	RID material;
	uint32_t users = 0;
};

std::map<FilterMaterialKey, FilterMaterial> g_filter_materials;

FilterMaterialKey filter_material_key(
		const ui::SwfFilter &p_f, const ui::SwfFilterKernel &p_kernel, double p_scale_x, double p_scale_y) {
	const Color color = color_of(p_f.color);
	return FilterMaterialKey{ float(p_f.kind), color.r, color.g, color.b, color.a,
		float(double(p_f.blur_x) * p_scale_x), float(double(p_f.blur_y) * p_scale_y), float(p_f.passes),
		float(p_kernel.shift_x), float(p_kernel.shift_y), p_f.strength, p_f.inner ? 1.0f : 0.0f,
		p_f.knockout ? 1.0f : 0.0f, p_f.composite_source ? 1.0f : 0.0f };
}

RID acquire_filter_material(RenderingServer *p_rs, RID p_shader, const FilterMaterialKey &p_key,
		const ui::SwfFilter &p_f, const ui::SwfFilterKernel &p_kernel) {
	FilterMaterial &entry = g_filter_materials[p_key];
	++entry.users;
	++g_counters.live_filter_stages;
	if (entry.material.is_valid()) {
		return entry.material;
	}
	entry.material = p_rs->material_create();
	++g_counters.live_filter_materials;
	p_rs->material_set_shader(entry.material, p_shader);
	PackedFloat32Array ax;
	PackedFloat32Array ay;
	ax.resize(kKernelSlots);
	ay.resize(kKernelSlots);
	for (int32_t i = 0; i < kKernelSlots; ++i) {
		ax.set(i, i < int32_t(p_kernel.x.size()) ? float(p_kernel.x[size_t(i)]) : 0.0f);
		ay.set(i, i < int32_t(p_kernel.y.size()) ? float(p_kernel.y[size_t(i)]) : 0.0f);
	}
	p_rs->material_set_param(entry.material, "shift", Vector2(float(p_kernel.shift_x), float(p_kernel.shift_y)));
	p_rs->material_set_param(entry.material, "taps_x", int32_t(p_kernel.x.size()));
	p_rs->material_set_param(entry.material, "taps_y", int32_t(p_kernel.y.size()));
	p_rs->material_set_param(entry.material, "weights_x", ax);
	p_rs->material_set_param(entry.material, "weights_y", ay);
	p_rs->material_set_param(entry.material, "shadow", vec4_of(color_of(p_f.color)));
	p_rs->material_set_param(entry.material, "strength", float(p_f.strength));
	p_rs->material_set_param(entry.material, "inner", p_f.inner ? 1 : 0);
	p_rs->material_set_param(entry.material, "knockout", p_f.knockout ? 1 : 0);
	p_rs->material_set_param(entry.material, "source", p_f.composite_source ? 1 : 0);
	p_rs->material_set_param(entry.material, "blur", p_f.kind == ui::SwfFilterKind::Blur ? 1 : 0);
	return entry.material;
}

void release_filter_material(RenderingServer *p_rs, const FilterMaterialKey &p_key) {
	const auto it = g_filter_materials.find(p_key);
	if (it == g_filter_materials.end()) {
		return;
	}
	--g_counters.live_filter_stages;
	if (--it->second.users == 0) {
		p_rs->free_rid(it->second.material);
		--g_counters.live_filter_materials;
		g_filter_materials.erase(it);
	}
}

constexpr float kSwfColorAddUnit = 255.0f;

Color shader_mul(const ui::SwfColorXform &p_cx) {
	return Color(p_cx.mul[0], p_cx.mul[1], p_cx.mul[2], p_cx.mul[3]);
}

Color shader_add(const ui::SwfColorXform &p_cx) {
	return Color(p_cx.add[0] / kSwfColorAddUnit, p_cx.add[1] / kSwfColorAddUnit, p_cx.add[2] / kSwfColorAddUnit, p_cx.add[3] / kSwfColorAddUnit);
}

void push_polygon(std::vector<ui::SwfPoint> &r_outline, std::vector<uint32_t> &r_ends, std::vector<ui::SwfPoint> p_poly) {
	if (p_poly.size() < 3) {
		return;
	}
	double area = 0.0;
	for (size_t i = 0; i < p_poly.size(); ++i) {
		const ui::SwfPoint &a = p_poly[i];
		const ui::SwfPoint &b = p_poly[(i + 1) % p_poly.size()];
		area += double(a.x) * b.y - double(b.x) * a.y;
	}
	if (std::abs(area) < 1e-12) {
		return;
	}
	if (area < 0.0) {
		std::reverse(p_poly.begin(), p_poly.end());
	}
	r_outline.insert(r_outline.end(), p_poly.begin(), p_poly.end());
	r_ends.push_back(uint32_t(r_outline.size()));
}

std::vector<ui::SwfPoint> circle(ui::SwfPoint p_c, double p_r) {
	const double r = std::max(p_r, 1e-4);
	const double step = std::acos(std::clamp(1.0 - kRoundTolerance / std::max(r, kRoundTolerance), -1.0, 1.0));
	const int32_t n = std::clamp(int32_t(std::ceil(kPi / std::max(step, 1e-3))), 6, 64);
	std::vector<ui::SwfPoint> out;
	out.reserve(size_t(n));
	for (int32_t i = 0; i < n; ++i) {
		const double a = 2.0 * kPi * double(i) / double(n);
		out.push_back({ float(p_c.x + r * std::cos(a)), float(p_c.y + r * std::sin(a)) });
	}
	return out;
}

void stroke_path(std::span<const ui::SwfPoint> p_pts, bool p_closed, double p_hw, const ui::SwfLineStyle &p_line, std::vector<ui::SwfPoint> &r_outline,
		std::vector<uint32_t> &r_ends) {
	std::vector<ui::SwfPoint> pts;
	for (const ui::SwfPoint &p : p_pts) {
		if (pts.empty() || pts.back() != p) {
			pts.push_back(p);
		}
	}
	if (p_closed && pts.size() > 1 && pts.front() == pts.back()) {
		pts.pop_back();
	}
	if (pts.size() == 1) {
		if (p_line.start_cap == 0) {
			push_polygon(r_outline, r_ends, circle(pts[0], p_hw));
		}
		return;
	}
	if (pts.size() < 2) {
		return;
	}
	const size_t n = pts.size();
	const size_t segs = p_closed ? n : n - 1;
	auto dir = [&](size_t i) {
		const ui::SwfPoint &a = pts[i % n];
		const ui::SwfPoint &b = pts[(i + 1) % n];
		const double dx = double(b.x) - a.x;
		const double dy = double(b.y) - a.y;
		const double l = std::sqrt(dx * dx + dy * dy);
		return std::array<double, 2>{ dx / l, dy / l };
	};
	for (size_t i = 0; i < segs; ++i) {
		const ui::SwfPoint &a = pts[i % n];
		const ui::SwfPoint &b = pts[(i + 1) % n];
		const std::array<double, 2> d = dir(i);
		double ax = a.x;
		double ay = a.y;
		double bx = b.x;
		double by = b.y;
		if (!p_closed && p_line.start_cap == 2 && i == 0) {
			ax -= d[0] * p_hw;
			ay -= d[1] * p_hw;
		}
		if (!p_closed && p_line.end_cap == 2 && i + 1 == segs) {
			bx += d[0] * p_hw;
			by += d[1] * p_hw;
		}
		const double nx = -d[1] * p_hw;
		const double ny = d[0] * p_hw;
		push_polygon(r_outline, r_ends,
				{ { float(ax + nx), float(ay + ny) }, { float(bx + nx), float(by + ny) }, { float(bx - nx), float(by - ny) }, { float(ax - nx), float(ay - ny) } });
	}
	const size_t first_join = p_closed ? 0 : 1;
	const size_t last_join = p_closed ? n : n - 1;
	for (size_t j = first_join; j < last_join; ++j) {
		const ui::SwfPoint &p = pts[j];
		if (p_line.join == 0) {
			push_polygon(r_outline, r_ends, circle(p, p_hw));
			continue;
		}
		const std::array<double, 2> d1 = dir((j + n - 1) % n);
		const std::array<double, 2> d2 = dir(j);
		const double cross = d1[0] * d2[1] - d1[1] * d2[0];
		const double side = cross > 0.0 ? -1.0 : 1.0;
		const double n1x = -d1[1] * p_hw * side;
		const double n1y = d1[0] * p_hw * side;
		const double n2x = -d2[1] * p_hw * side;
		const double n2y = d2[0] * p_hw * side;
		const ui::SwfPoint e1{ float(p.x + n1x), float(p.y + n1y) };
		const ui::SwfPoint e2{ float(p.x + n2x), float(p.y + n2y) };
		bool mitred = false;
		if (p_line.join == 2) {
			const double mx = n1x + n2x;
			const double my = n1y + n2y;
			const double ml = std::sqrt(mx * mx + my * my);
			const double cos_half = ml / (2.0 * p_hw);
			if (cos_half > 1e-6) {
				const double len = p_hw / cos_half;
				if (len <= std::max(1.0, double(p_line.miter_limit)) * p_hw) {
					const ui::SwfPoint m{ float(p.x + mx / ml * len), float(p.y + my / ml * len) };
					push_polygon(r_outline, r_ends, { p, e1, m, e2 });
					mitred = true;
				}
			}
		}
		if (!mitred) {
			push_polygon(r_outline, r_ends, { p, e1, e2 });
		}
	}
	if (!p_closed) {
		if (p_line.start_cap == 0) {
			push_polygon(r_outline, r_ends, circle(pts.front(), p_hw));
		}
		if (p_line.end_cap == 0) {
			push_polygon(r_outline, r_ends, circle(pts.back(), p_hw));
		}
	}
}

void flatten_glyph_contour(const std::vector<ui::GlyphPoint> &p_points, size_t p_first, size_t p_last, std::vector<ui::SwfPoint> &r_out) {
	const size_t n = p_last - p_first + 1;
	auto pt = [&](size_t i) -> const ui::GlyphPoint & { return p_points[p_first + (i % n)]; };
	size_t s = 0;
	while (s < n && !pt(s).on_curve) {
		++s;
	}
	ui::SwfPoint cur;
	if (s == n) {
		cur = { float((pt(0).x + pt(1).x) * 0.5), float((pt(0).y + pt(1).y) * 0.5) };
		s = 0;
	} else {
		cur = { float(pt(s).x), float(pt(s).y) };
		s = s + 1;
	}
	const ui::SwfPoint start = cur;
	r_out.push_back(cur);
	for (size_t k = 0; k < n; ++k) {
		const ui::GlyphPoint &p = pt(s + k);
		if (p.on_curve) {
			cur = { float(p.x), float(p.y) };
			if (!(k + 1 == n && cur == start)) {
				r_out.push_back(cur);
			}
			continue;
		}
		const ui::GlyphPoint &q = pt(s + k + 1);
		ui::SwfPoint end;
		if (q.on_curve) {
			end = { float(q.x), float(q.y) };
			++k;
		} else {
			end = { float((p.x + q.x) * 0.5), float((p.y + q.y) * 0.5) };
		}
		const ui::SwfPoint ctrl{ float(p.x), float(p.y) };
		const uint32_t segs = ui::swf_curve_segments(cur, ctrl, end, kGlyphFlattenUnits);
		for (uint32_t i = 1; i <= segs; ++i) {
			const double t = double(i) / double(segs);
			const double u = 1.0 - t;
			const ui::SwfPoint v{ float(u * u * cur.x + 2.0 * u * t * ctrl.x + t * t * end.x), float(u * u * cur.y + 2.0 * u * t * ctrl.y + t * t * end.y) };
			if (!(i == segs && k + 1 >= n && v == start)) {
				r_out.push_back(v);
			}
		}
		cur = end;
	}
}

struct Vert {
	ui::SwfPoint p;
	ui::SwfPoint uv;
	Color color;
};

Vert lerp_vert(const Vert &p_a, const Vert &p_b, double p_t) {
	Vert v;
	v.p = { float(p_a.p.x + (p_b.p.x - p_a.p.x) * p_t), float(p_a.p.y + (p_b.p.y - p_a.p.y) * p_t) };
	v.uv = { float(p_a.uv.x + (p_b.uv.x - p_a.uv.x) * p_t), float(p_a.uv.y + (p_b.uv.y - p_a.uv.y) * p_t) };
	v.color = p_a.color.lerp(p_b.color, float(p_t));
	return v;
}

std::vector<Vert> split_polygon(const std::vector<Vert> &p_poly, bool p_x_axis, double p_at, bool p_keep_below) {
	std::vector<Vert> out;
	const size_t n = p_poly.size();
	for (size_t i = 0; i < n; ++i) {
		const Vert &a = p_poly[i];
		const Vert &b = p_poly[(i + 1) % n];
		const double va = (p_x_axis ? a.p.x : a.p.y) - p_at;
		const double vb = (p_x_axis ? b.p.x : b.p.y) - p_at;
		const bool ia = p_keep_below ? va <= 0.0 : va >= 0.0;
		const bool ib = p_keep_below ? vb <= 0.0 : vb >= 0.0;
		if (ia) {
			out.push_back(a);
		}
		if (ia != ib && va != vb) {
			out.push_back(lerp_vert(a, b, va / (va - vb)));
		}
	}
	return out;
}

constexpr size_t kPrimitiveMaxTriangles = 32;

struct Geometry {
	std::vector<Vert> verts;
	std::vector<int32_t> indices;
	void add_triangle(const Vert &p_a, const Vert &p_b, const Vert &p_c) {
		const int32_t base = int32_t(verts.size());
		verts.push_back(p_a);
		verts.push_back(p_b);
		verts.push_back(p_c);
		indices.push_back(base);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
	}
	void add_warped(const Vert &p_a, const Vert &p_b, const Vert &p_c, const ui::SwfGridWarp &p_grid) {
		if (!p_grid.active) {
			add_triangle(p_a, p_b, p_c);
			return;
		}
		std::vector<std::vector<Vert>> pieces{ { p_a, p_b, p_c } };
		const std::array<std::pair<bool, double>, 4> cuts{ { { true, p_grid.l }, { true, p_grid.r }, { false, p_grid.t }, { false, p_grid.b } } };
		for (const auto &[x_axis, at] : cuts) {
			std::vector<std::vector<Vert>> next;
			for (const std::vector<Vert> &poly : pieces) {
				std::vector<Vert> lo = split_polygon(poly, x_axis, at, true);
				std::vector<Vert> hi = split_polygon(poly, x_axis, at, false);
				if (lo.size() >= 3) {
					next.push_back(std::move(lo));
				}
				if (hi.size() >= 3) {
					next.push_back(std::move(hi));
				}
			}
			pieces = std::move(next);
		}
		for (const std::vector<Vert> &poly : pieces) {
			for (size_t i = 1; i + 1 < poly.size(); ++i) {
				Vert a = poly[0];
				Vert b = poly[i];
				Vert c = poly[i + 1];
				a.p = p_grid.warp(a.p);
				b.p = p_grid.warp(b.p);
				c.p = p_grid.warp(c.p);
				add_triangle(a, b, c);
			}
		}
	}
};

uint64_t target_bytes(Vector2i p_size, RenderingServer::ViewportMSAA p_msaa) {
	uint64_t samples = 1;
	if (p_msaa == RenderingServer::VIEWPORT_MSAA_2X) {
		samples = 3;
	} else if (p_msaa == RenderingServer::VIEWPORT_MSAA_4X) {
		samples = 5;
	} else if (p_msaa == RenderingServer::VIEWPORT_MSAA_8X) {
		samples = 9;
	}
	return ui::swf_target_bytes(p_size.x, p_size.y, samples);
}

RID make_viewport(RenderingServer *p_rs, RenderingServer::ViewportMSAA p_msaa) {
	const RID viewport = p_rs->viewport_create();
	++g_counters.viewport_creates;
	++g_counters.live_surfaces;
	p_rs->viewport_set_disable_3d(viewport, true);
	p_rs->viewport_set_transparent_background(viewport, true);
	p_rs->viewport_set_clear_mode(viewport, RenderingServer::VIEWPORT_CLEAR_ALWAYS);
	p_rs->viewport_set_update_mode(viewport, RenderingServer::VIEWPORT_UPDATE_DISABLED);
	p_rs->viewport_set_msaa_2d(viewport, p_msaa);
	p_rs->viewport_set_use_hdr_2d(viewport, false);
	return viewport;
}

struct SpareSurface {
	RID viewport;
	RID texture;
	Vector2i size;
	RenderingServer::ViewportMSAA msaa = RenderingServer::VIEWPORT_MSAA_DISABLED;
};

struct Surface {
	RID viewport;
	RID canvas;
	RID texture;
	Vector2i size;

	void create(RenderingServer *p_rs, Vector2i p_size, RID p_parent, RenderingServer::ViewportMSAA p_msaa) {
		viewport = make_viewport(p_rs, p_msaa);
		if (p_parent.is_valid()) {
			p_rs->viewport_set_parent_viewport(viewport, p_parent);
		}
		p_rs->viewport_set_active(viewport, true);
		canvas = p_rs->canvas_create();
		p_rs->viewport_attach_canvas(viewport, canvas);
		size = Vector2i();
		if (p_size.x > 0 && p_size.y > 0) {
			resize(p_rs, p_size);
		}
		texture = p_rs->viewport_get_texture(viewport);
	}

	void create_canvas_only(RenderingServer *p_rs) {
		canvas = p_rs->canvas_create();
		size = Vector2i();
	}

	bool resize(RenderingServer *p_rs, Vector2i p_size) {
		const Vector2i s(std::max(1, p_size.x), std::max(1, p_size.y));
		if (s == size) {
			return false;
		}
		size = s;
		p_rs->viewport_set_size(viewport, size.x, size.y);
		++g_counters.viewport_resizes;
		return true;
	}

	uint64_t bytes(RenderingServer::ViewportMSAA p_msaa) const { return target_bytes(size, p_msaa); }

	void render(RenderingServer *p_rs) const {
		if (viewport.is_valid()) {
			++g_counters.viewport_render_requests;
			p_rs->viewport_set_update_mode(viewport, RenderingServer::VIEWPORT_UPDATE_ONCE);
		}
	}

	void free(RenderingServer *p_rs) {
		if (canvas.is_valid()) {
			p_rs->free_rid(canvas);
		}
		if (viewport.is_valid()) {
			p_rs->free_rid(viewport);
			--g_counters.live_surfaces;
		}
		canvas = RID();
		viewport = RID();
		texture = RID();
	}
};

struct Applied {
	bool valid = false;
	Transform2D xform;
	std::array<Vector3, 3> h;
	Color mul;
	Color add;
	int32_t flags = -1;
	int32_t mode = -1;
	Color uv;
	Vector2 inv;
	bool visible = false;
};

struct Piece {
	RID item;
	int32_t mode = kModeSolid;
	Color uv;
	Vector2 inv{ 1.0f, 1.0f };
	bool wrap = false;
	bool smooth = true;
	bool additive = false;
	Ref<Texture2D> texture;
	ui::SwfRect bounds;
	bool has_bounds = false;
	RID parent;
	int32_t index = -1;
	Applied applied;
	bool has_array = false;
};

enum class CanvasKind : uint8_t {
	Root,
	Filter,
	Mask,
	Blend,
};

struct Canvas;

struct Stage {
	Surface surface;
	RID item;
	FilterMaterialKey material_key{};
};

struct Canvas {
	CanvasKind kind = CanvasKind::Root;
	ui::SwfNodeId owner = ui::kSwfNoNode;
	Canvas *parent = nullptr;
	ui::SwfNodeId tilt_floor = ui::kSwfNoNode;
	ui::SwfNodeId color_floor = ui::kSwfNoNode;
	Surface content;
	Surface alpha;
	std::vector<Stage> stages;
	std::vector<ui::SwfFilter> filters;
	RID quad;
	RID quad_material;
	Applied quad_applied;
	RID quad_parent;
	int32_t quad_index = -1;
	Vector2i origin;
	Vector2i size{ 1, 1 };
	Vector2i extent{ 1, 1 };
	bool has_content = false;
	bool used = false;
	bool render_dirty = true;
	bool bounds_dirty = true;
	bool additive = false;
	int32_t next_index = 0;
	int32_t next_alpha_index = 0;
	double margin_x = 0.0;
	double margin_y = 0.0;
	std::vector<ui::SwfNodeId> nodes;
	std::vector<ui::SwfNodeId> alpha_nodes;
	std::vector<Canvas *> children;
	std::vector<ui::SwfNodeId> targets;
	std::vector<ui::SwfNodeId> old_nodes;
	std::vector<ui::SwfNodeId> old_alpha_nodes;
	std::vector<Canvas *> old_children;
	std::vector<ui::SwfNodeId> old_targets;
	bool changed = true;
	RID linked_parent;
	size_t linked_stages = size_t(-1);
	Vector2i quad_origin{ -1, -1 };
	Vector2i quad_size{ -1, -1 };
	RID quad_texture;
	int32_t quad_filter = -1;
	uint64_t owner_path = 0;
	uint64_t parked = 0;
	bool hidden = false;

	const Surface &final_surface() const { return stages.empty() ? content : stages.back().surface; }
};

struct BuildCounts {
	uint32_t glyphs = 0;
	uint32_t missing_glyphs = 0;
	uint32_t missing_fonts = 0;
	uint32_t missing_textures = 0;
	uint32_t unsupported_fills = 0;
	uint32_t unsupported_lines = 0;
};

struct NodeRec {
	std::vector<Piece> pieces;
	BuildCounts counts;
	bool built = false;
	bool reopened = false;
	uint64_t fingerprint = 0;
	uint32_t text_revision = 0;
	uint32_t character_revision = 0;
	const void *content = nullptr;
	Canvas *canvas = nullptr;
	bool in_alpha = false;
	bool drawn = false;
	bool additive = false;
	bool seen = false;
	bool grid = false;
	ui::SwfAffine built_to_owner;
	double built_scale_x = 1.0;
	double built_scale_y = 1.0;
	bool scale_dependent = false;
	bool content_dirty = true;
	Canvas *layer = nullptr;
	uint64_t path_hash = 0;
	uint64_t parked = 0;
	ui::SwfNodeId id = ui::kSwfNoNode;
	bool hidden = false;
};

struct Target {
	std::string renderer;
	std::string path;
	ui::SwfNodeId node = ui::kSwfNoNode;
	Surface surface;
	RID root_item;
	RID quad;
	Applied quad_applied;
	RID quad_parent;
	int32_t quad_index = -1;
	Canvas *canvas = nullptr;
	ui::SwfRect local;
	ui::SwfRect drawn_local{ -1.0f, -1.0f, -1.0f, -1.0f };
	Vector2i drawn_size;
	bool live = false;
	bool running = false;
	bool used = false;
	bool redraw = true;
};

struct GlyphMesh {
	std::vector<ui::SwfPoint> vertices;
	std::vector<uint32_t> indices;
	bool ok = false;
};

uint64_t canvas_key(CanvasKind p_kind, ui::SwfNodeId p_owner) {
	return (uint64_t(p_kind) << 32) | uint64_t(p_owner);
}

uint64_t path_hash(const ui::SwfSceneNode &p_n) {
	uint64_t h = bf3::core::kFnv1aOffset;
	bf3::core::fnv1a(h, p_n.path.data(), p_n.path.size());
	return h;
}

uint64_t dormant_key(CanvasKind p_kind, uint64_t p_path) {
	uint64_t h = p_path;
	bf3::core::fnv1a_value(h, p_kind);
	return h;
}

}

struct Bf3SwfView::Impl {
	RenderingServer *rs = nullptr;
	Bf3SwfView *owner = nullptr;
	Bf3UiAssets *assets = nullptr;
	std::shared_ptr<const ui::SwfLibrary> library;
	std::shared_ptr<Bf3UiTextures> textures;
	std::optional<ui::SwfScene> scene;
	bool skips_hidden_render = false;
	std::string movie;
	std::string error;
	bool playing = true;
	SwfScaleMode scale_mode = SwfScaleMode::ShowAll;
	bool explicit_size = false;
	Rect2 present_rect{ 0.0f, 0.0f, 1280.0f, 720.0f };
	Vector2i target_size{ 1280, 720 };
	RenderingServer::ViewportMSAA msaa = RenderingServer::VIEWPORT_MSAA_4X;
	std::vector<SpareSurface> spares;
	uint64_t open_serial = 0;
	SwfStageFit fit;
	ui::SwfAffine stage_to_window;
	bool filters_enabled = true;
	std::string isolated;
	ui::SwfNodeId isolated_id = ui::kSwfNoNode;
	std::set<ui::SwfNodeId> isolated_ancestors;
	std::map<std::string, SwfTilt> tilt_paths;
	std::unordered_map<ui::SwfNodeId, SwfTilt> tilts;
	std::map<std::string, std::string> renderer_paths;
	std::unordered_map<ui::SwfNodeId, std::string> renderers;
	std::unordered_map<ui::SwfNodeId, NodeRec> recs;
	std::map<uint64_t, Canvas> canvases;
	std::unordered_multimap<uint64_t, NodeRec> dormant_recs;
	std::multimap<uint64_t, Canvas> dormant_canvases;
	uint64_t dormant_serial = 0;
	std::unordered_map<ui::SwfNodeId, ui::SwfNodeId> adopted_ids;
	uint32_t settle_syncs = 0;
	std::vector<Canvas *> order;
	std::map<ui::SwfNodeId, Target> targets;
	std::map<std::pair<const ui::SwfFontFace *, uint16_t>, GlyphMesh> glyphs;
	std::set<std::string> logged;
	RID item_shader;
	RID item_material;
	RID filter_shader;
	RID present_shader;
	RID present_material;
	RID present_item;
	RID owner_viewport;
	bool structure_dirty = true;
	uint32_t items_created = 0;
	SwfViewStats stats;

	Impl() {
		rs = RenderingServer::get_singleton();
		const SharedShaders &shaders = acquire_shaders(rs);
		item_shader = shaders.item;
		filter_shader = shaders.filter;
		present_shader = shaders.present;
		item_material = rs->material_create();
		rs->material_set_shader(item_material, item_shader);
		present_material = rs->material_create();
		rs->material_set_shader(present_material, present_shader);
	}

	~Impl() {
		release();
		rs->free_rid(present_material);
		rs->free_rid(item_material);
		release_shaders(rs);
	}

	void warn_once(const std::string &p_message) {
		if (logged.insert(p_message).second) {
			UtilityFunctions::push_warning(to_godot("[swf view] " + movie + ": " + p_message));
		}
	}

	void park(Surface &r_s, RenderingServer::ViewportMSAA p_msaa) {
		if (!r_s.viewport.is_valid()) {
			return;
		}
		if (r_s.size.x <= 0 || r_s.size.y <= 0) {
			rs->free_rid(r_s.viewport);
			--g_counters.live_surfaces;
			r_s.viewport = RID();
			r_s.texture = RID();
			return;
		}
		if (r_s.canvas.is_valid()) {
			rs->viewport_remove_canvas(r_s.viewport, r_s.canvas);
		}
		rs->viewport_set_parent_viewport(r_s.viewport, RID());
		rs->viewport_set_update_mode(r_s.viewport, RenderingServer::VIEWPORT_UPDATE_DISABLED);
		spares.push_back(SpareSurface{ r_s.viewport, r_s.texture, r_s.size, p_msaa });
		r_s.viewport = RID();
		r_s.texture = RID();
		r_s.size = Vector2i();
		uint64_t bytes = 0;
		size_t kept = 0;
		for (auto it = spares.rbegin(); it != spares.rend();) {
			bytes += target_bytes(it->size, it->msaa);
			if (bytes > Bf3SwfView::kSpareSurfaceBytes || ++kept > Bf3SwfView::kSpareSurfaces) {
				rs->free_rid(it->viewport);
				--g_counters.live_surfaces;
				it = std::make_reverse_iterator(spares.erase(std::next(it).base()));
			} else {
				++it;
			}
		}
	}

	void retire(Surface &r_s, RenderingServer::ViewportMSAA p_msaa) {
		park(r_s, p_msaa);
		r_s.free(rs);
	}

	void free_spares() {
		for (const SpareSurface &sp : spares) {
			rs->free_rid(sp.viewport);
			--g_counters.live_surfaces;
		}
		spares.clear();
	}

	bool take(Surface &r_s, Vector2i p_size, RenderingServer::ViewportMSAA p_msaa) {
		if (r_s.viewport.is_valid() && r_s.size == p_size) {
			return false;
		}
		const auto exact = [&](const SpareSurface &p_sp) { return p_sp.msaa == p_msaa && p_sp.size == p_size; };
		const bool spare_exact = std::any_of(spares.begin(), spares.end(), exact);
		if (!spare_exact && r_s.viewport.is_valid() && r_s.size.x > 0 && r_s.size.y > 0) {
			r_s.resize(rs, p_size);
			return false;
		}
		park(r_s, p_msaa);
		size_t pick = spares.size();
		for (size_t i = spares.size(); i-- > 0;) {
			if (exact(spares[i]) || (pick == spares.size() && spares[i].msaa == p_msaa)) {
				pick = i;
				if (exact(spares[i])) {
					break;
				}
			}
		}
		if (pick < spares.size()) {
			r_s.viewport = spares[pick].viewport;
			r_s.texture = spares[pick].texture;
			r_s.size = spares[pick].size;
			spares.erase(spares.begin() + std::ptrdiff_t(pick));
		} else {
			r_s.viewport = make_viewport(rs, p_msaa);
			rs->viewport_set_active(r_s.viewport, true);
			r_s.texture = rs->viewport_get_texture(r_s.viewport);
			r_s.size = Vector2i();
		}
		rs->viewport_attach_canvas(r_s.viewport, r_s.canvas);
		r_s.resize(rs, p_size);
		return true;
	}

	RID make_item() {
		++g_counters.live_items;
		return rs->canvas_item_create();
	}

	void free_item(RID p_item) {
		--g_counters.live_items;
		rs->free_rid(p_item);
	}

	void free_piece(const Piece &p_piece) {
		g_counters.live_triangle_arrays -= p_piece.has_array ? 1u : 0u;
		free_item(p_piece.item);
	}

	void free_canvas(Canvas &r_c) {
		for (Stage &s : r_c.stages) {
			if (s.item.is_valid()) {
				free_item(s.item);
			}
			release_filter_material(rs, s.material_key);
			retire(s.surface, RenderingServer::VIEWPORT_MSAA_DISABLED);
		}
		r_c.stages.clear();
		if (r_c.quad.is_valid()) {
			free_item(r_c.quad);
		}
		if (r_c.quad_material.is_valid()) {
			rs->free_rid(r_c.quad_material);
		}
		r_c.quad = RID();
		r_c.quad_material = RID();
		if (r_c.kind == CanvasKind::Root) {
			r_c.content.free(rs);
		} else {
			retire(r_c.content, msaa);
		}
		retire(r_c.alpha, msaa);
	}

	void set_target_running(Target &r_t, bool p_running) {
		if (r_t.running == p_running) {
			return;
		}
		r_t.running = p_running;
		if (p_running) {
			++g_counters.live_targets_running;
			++g_counters.viewport_render_requests;
		} else {
			--g_counters.live_targets_running;
		}
		rs->viewport_set_update_mode(r_t.surface.viewport,
				p_running ? RenderingServer::VIEWPORT_UPDATE_ALWAYS : RenderingServer::VIEWPORT_UPDATE_DISABLED);
	}

	void free_target(Target &r_t) {
		set_target_running(r_t, false);
		if (r_t.root_item.is_valid()) {
			free_item(r_t.root_item);
		}
		if (r_t.quad.is_valid()) {
			free_item(r_t.quad);
		}
		r_t.root_item = RID();
		r_t.quad = RID();
		r_t.surface.free(rs);
	}

	void free_rec(NodeRec &r_rec) {
		for (Piece &p : r_rec.pieces) {
			if (p.item.is_valid()) {
				free_piece(p);
			}
		}
		r_rec.pieces.clear();
	}

	uint64_t canvas_bytes(const Canvas &p_c) const {
		uint64_t out = p_c.content.bytes(msaa) + p_c.alpha.bytes(msaa);
		for (const Stage &s : p_c.stages) {
			out += s.surface.bytes(RenderingServer::VIEWPORT_MSAA_DISABLED);
		}
		return out;
	}

	void park_rec(NodeRec &&r_rec) {
		if (r_rec.path_hash == 0) {
			free_rec(r_rec);
			return;
		}
		r_rec.hidden = false;
		r_rec.canvas = nullptr;
		r_rec.layer = nullptr;
		r_rec.drawn = false;
		r_rec.seen = false;
		r_rec.parked = ++dormant_serial;
		dormant_recs.emplace(r_rec.path_hash, std::move(r_rec));
	}

	std::map<uint64_t, Canvas>::iterator park_canvas(std::map<uint64_t, Canvas>::iterator p_it) {
		const auto next = std::next(p_it);
		if (p_it->second.kind == CanvasKind::Root || p_it->second.owner_path == 0) {
			free_canvas(p_it->second);
			canvases.erase(p_it);
			return next;
		}
		auto handle = canvases.extract(p_it);
		Canvas &c = handle.mapped();
		for (const RID vp : { c.content.viewport, c.alpha.viewport }) {
			if (vp.is_valid()) {
				rs->viewport_set_parent_viewport(vp, RID());
			}
		}
		for (const Stage &s : c.stages) {
			if (s.surface.viewport.is_valid()) {
				rs->viewport_set_parent_viewport(s.surface.viewport, RID());
			}
		}
		c.linked_parent = RID();
		c.linked_stages = size_t(-1);
		c.used = false;
		c.hidden = false;
		c.parked = ++dormant_serial;
		handle.key() = dormant_key(c.kind, c.owner_path);
		dormant_canvases.insert(std::move(handle));
		return next;
	}

	void hide_dormant() {
		for (auto &[k, c] : dormant_canvases) {
			if (!c.hidden && c.quad.is_valid()) {
				rs->canvas_item_set_visible(c.quad, false);
				c.quad_applied.visible = false;
			}
			c.hidden = true;
		}
		for (auto &[k, r] : dormant_recs) {
			if (!r.hidden) {
				for (Piece &p : r.pieces) {
					rs->canvas_item_set_visible(p.item, false);
					p.applied.visible = false;
				}
			}
			r.hidden = true;
		}
	}

	bool same_ids(const std::vector<ui::SwfNodeId> &p_now, const std::vector<ui::SwfNodeId> &p_before) const {
		if (p_now.size() != p_before.size()) {
			return false;
		}
		for (size_t i = 0; i < p_now.size(); ++i) {
			const auto t = adopted_ids.find(p_before[i]);
			if ((t != adopted_ids.end() ? t->second : p_before[i]) != p_now[i]) {
				return false;
			}
		}
		return true;
	}

	void trim_dormant() {
		if (settle_syncs < Bf3SwfView::kDormantSettleSyncs) {
			return;
		}
		uint64_t bytes = 0;
		size_t items = dormant_canvases.size() + dormant_recs.size();
		for (const auto &[k, c] : dormant_canvases) {
			bytes += canvas_bytes(c);
		}
		for (const auto &[k, r] : dormant_recs) {
			items += r.pieces.size();
		}
		if (bytes <= Bf3SwfView::kDormantSurfaceBytes && items <= Bf3SwfView::kDormantItems) {
			return;
		}
		std::vector<std::multimap<uint64_t, Canvas>::iterator> old_canvases;
		for (auto it = dormant_canvases.begin(); it != dormant_canvases.end(); ++it) {
			old_canvases.push_back(it);
		}
		std::vector<std::unordered_multimap<uint64_t, NodeRec>::iterator> old_recs;
		for (auto it = dormant_recs.begin(); it != dormant_recs.end(); ++it) {
			old_recs.push_back(it);
		}
		std::sort(old_canvases.begin(), old_canvases.end(),
				[](const auto &p_a, const auto &p_b) { return p_a->second.parked < p_b->second.parked; });
		std::sort(old_recs.begin(), old_recs.end(),
				[](const auto &p_a, const auto &p_b) { return p_a->second.parked < p_b->second.parked; });
		size_t ci = 0;
		size_t ri = 0;
		while ((bytes > Bf3SwfView::kDormantSurfaceBytes || items > Bf3SwfView::kDormantItems) &&
				(ci < old_canvases.size() || ri < old_recs.size())) {
			const bool canvas_first = ri == old_recs.size() ||
					(ci < old_canvases.size() && old_canvases[ci]->second.parked < old_recs[ri]->second.parked);
			if (canvas_first) {
				const auto it = old_canvases[ci++];
				bytes -= canvas_bytes(it->second);
				items -= 1;
				free_canvas(it->second);
				dormant_canvases.erase(it);
			} else {
				const auto it = old_recs[ri++];
				items -= it->second.pieces.size() + 1;
				free_rec(it->second);
				dormant_recs.erase(it);
			}
		}
	}

	void recycle_dormant(CanvasKind p_kind) {
		auto pick = dormant_canvases.end();
		for (auto it = dormant_canvases.begin(); it != dormant_canvases.end(); ++it) {
			if (it->second.kind == p_kind &&
					(pick == dormant_canvases.end() || it->second.parked < pick->second.parked)) {
				pick = it;
			}
		}
		if (pick != dormant_canvases.end()) {
			free_canvas(pick->second);
			dormant_canvases.erase(pick);
		}
	}

	void free_dormant() {
		for (auto &[k, r] : dormant_recs) {
			free_rec(r);
		}
		dormant_recs.clear();
		for (auto &[k, c] : dormant_canvases) {
			free_canvas(c);
		}
		dormant_canvases.clear();
	}

	NodeRec &rec_for(ui::SwfNodeId p_id) {
		const auto it = recs.find(p_id);
		if (it != recs.end()) {
			return it->second;
		}
		NodeRec &rec = recs[p_id];
		if (const ui::SwfSceneNode *n = node(p_id)) {
			rec.path_hash = path_hash(*n);
			const auto d = dormant_recs.find(rec.path_hash);
			if (d != dormant_recs.end()) {
				rec = std::move(d->second);
				dormant_recs.erase(d);
				rec.reopened = true;
				if (const auto t = adopted_ids.find(rec.id); t != adopted_ids.end()) {
					t->second = p_id;
				}
			}
		}
		rec.id = p_id;
		return rec;
	}

	void release() {
		for (auto &[id, rec] : recs) {
			free_rec(rec);
		}
		recs.clear();
		free_dormant();
		for (auto &[id, t] : targets) {
			free_target(t);
		}
		targets.clear();
		for (auto &[k, c] : canvases) {
			free_canvas(c);
		}
		canvases.clear();
		order.clear();
		free_spares();
		if (present_item.is_valid()) {
			free_item(present_item);
			present_item = RID();
		}
		scene.reset();
		library.reset();
		structure_dirty = true;
	}

	void retire_scene() {
		adopted_ids.clear();
		for (auto &[id, rec] : recs) {
			if (const ui::SwfSceneNode *n = node(id)) {
				rec.path_hash = path_hash(*n);
			}
			rec.id = id;
			adopted_ids.emplace(id, ui::kSwfNoNode);
			park_rec(std::move(rec));
		}
		recs.clear();
		for (auto it = canvases.begin(); it != canvases.end();) {
			if (it->second.kind == CanvasKind::Root) {
				++it;
			} else {
				if (const ui::SwfSceneNode *n = node(it->second.owner)) {
					it->second.owner_path = path_hash(*n);
				}
				it = park_canvas(it);
			}
		}
		order.clear();
		tilt_paths.clear();
		renderer_paths.clear();
		scene.reset();
		structure_dirty = true;
	}

	const ui::SwfSceneNode *node(ui::SwfNodeId p_id) const { return scene ? scene->node(p_id) : nullptr; }

	void update_fit() {
		const ui::SwfRect st = scene ? scene->stage() : ui::SwfRect{ 0.0f, 1280.0f, 0.0f, 720.0f };
		const double w = std::max(1.0, double(st.width()));
		const double h = std::max(1.0, double(st.height()));
		const double tw = double(target_size.x);
		const double th = double(target_size.y);
		double kx = tw / w;
		double ky = th / h;
		switch (scale_mode) {
			case SwfScaleMode::ShowAll: {
				const ui::UiStage s = ui::ui_stage_show_all(st, float(tw), float(th));
				fit = SwfStageFit{ s.scale, s.scale, s.offset_x, s.offset_y };
				stage_to_window = ui::SwfAffine{ s.scale, 0.0, 0.0, s.scale, s.offset_x, s.offset_y };
				return;
			}
			case SwfScaleMode::NoBorder:
				kx = ky = std::max(kx, ky);
				break;
			case SwfScaleMode::NoScale:
				kx = ky = 1.0;
				break;
			case SwfScaleMode::ExactFit:
				break;
		}
		fit.scale_x = kx;
		fit.scale_y = ky;
		fit.offset_x = (tw - w * kx) * 0.5 - double(st.x_min) * kx;
		fit.offset_y = (th - h * ky) * 0.5 - double(st.y_min) * ky;
		stage_to_window = ui::SwfAffine{ kx, 0.0, 0.0, ky, fit.offset_x, fit.offset_y };
	}

	Hom window_tilt(ui::SwfNodeId p_id, const SwfTilt &p_tilt) const {
		const ui::SwfSceneNode *n = node(p_id);
		const Hom stage = tilt_homography(p_tilt, n->world.tx, n->world.ty, scene->stage());
		return hmul(hmul(hom_of(stage_to_window), stage), hom_of(ui::swf_inverse(stage_to_window).value_or(kSingularAffine)));
	}

	Hom tilt_chain(ui::SwfNodeId p_from, ui::SwfNodeId p_floor) const {
		Hom h;
		if (tilts.empty()) {
			return h;
		}
		for (ui::SwfNodeId a = p_from; a != ui::kSwfNoNode && a != p_floor;) {
			const auto it = tilts.find(a);
			if (it != tilts.end()) {
				h = hmul(window_tilt(a, it->second), h);
			}
			const ui::SwfSceneNode *n = node(a);
			a = n != nullptr ? n->parent : ui::kSwfNoNode;
		}
		return h;
	}

	ui::SwfColorXform relative_color(ui::SwfNodeId p_id, ui::SwfNodeId p_floor) const {
		const ui::SwfSceneNode *n = node(p_id);
		if (p_floor == ui::kSwfNoNode) {
			return n->world_color;
		}
		std::vector<const ui::SwfSceneNode *> path;
		for (const ui::SwfSceneNode *a = n; a != nullptr && a->id != p_floor; a = node(a->parent)) {
			path.push_back(a);
		}
		ui::SwfColorXform c;
		for (auto it = path.rbegin(); it != path.rend(); ++it) {
			c = ui::swf_concat(c, (*it)->color);
		}
		return c;
	}

	Ref<Texture2D> atlas(const ui::SwfSceneNode &p_node, uint16_t p_image_id) {
		if (assets == nullptr) {
			return Ref<Texture2D>();
		}
		if (!assets->ensure_atlas_movie(p_node.movie_name)) {
			warn_once("no atlas map for " + p_node.movie_name);
			return Ref<Texture2D>();
		}
		return assets->atlas_texture(p_node.movie_name, p_image_id);
	}

	const GlyphMesh &glyph_mesh(const ui::SwfFontFace *p_face, uint16_t p_glyph) {
		const auto key = std::make_pair(p_face, p_glyph);
		auto it = glyphs.find(key);
		if (it != glyphs.end()) {
			return it->second;
		}
		GlyphMesh &g = glyphs[key];
		if (p_glyph >= p_face->outlines.glyphs.size()) {
			return g;
		}
		ui::GlyphOutline outline = p_face->outlines.glyphs[p_glyph];
		bf3::core::level::WorkBudget budget(kGeometryBudget);
		if (!ui::normalise_winding(outline, budget)) {
			return g;
		}
		std::vector<ui::SwfPoint> pts;
		std::vector<uint32_t> ends;
		size_t first = 0;
		for (const uint32_t last : outline.contour_ends) {
			if (last < first || last >= outline.points.size()) {
				return g;
			}
			const size_t before = pts.size();
			flatten_glyph_contour(outline.points, first, last, pts);
			if (pts.size() - before < 3) {
				pts.resize(before);
			} else {
				ends.push_back(uint32_t(pts.size()));
			}
			first = size_t(last) + 1;
		}
		ui::SwfTriangulation tri;
		if (!ui::swf_triangulate_nonzero(pts, ends, budget, tri)) {
			return g;
		}
		g.vertices = std::move(tri.vertices);
		g.indices = std::move(tri.indices);
		g.ok = true;
		return g;
	}

	void emit_primitives(Piece &r_piece, const Geometry &p_geo, RID p_texture) {
		PackedVector2Array points;
		PackedVector2Array uvs;
		PackedColorArray colors;
		points.resize(3);
		uvs.resize(3);
		colors.resize(3);
		for (size_t i = 0; i + 2 < p_geo.indices.size(); i += 3) {
			Vector2 *point = points.ptrw();
			Vector2 *uv = uvs.ptrw();
			Color *color = colors.ptrw();
			for (size_t k = 0; k < 3; ++k) {
				const Vert &v = p_geo.verts[size_t(p_geo.indices[i + k])];
				point[k] = Vector2(v.p.x, v.p.y);
				uv[k] = Vector2(v.uv.x, v.uv.y);
				color[k] = v.color;
			}
			rs->canvas_item_add_primitive(r_piece.item, points, colors, uvs, p_texture);
		}
	}

	void emit_triangle_array(Piece &r_piece, const Geometry &p_geo, RID p_texture) {
		PackedVector2Array points;
		PackedVector2Array uvs;
		PackedColorArray colors;
		PackedInt32Array indices;
		points.resize(int64_t(p_geo.verts.size()));
		uvs.resize(int64_t(p_geo.verts.size()));
		colors.resize(int64_t(p_geo.verts.size()));
		indices.resize(int64_t(p_geo.indices.size()));
		Vector2 *point = points.ptrw();
		Vector2 *uv = uvs.ptrw();
		Color *color = colors.ptrw();
		for (size_t i = 0; i < p_geo.verts.size(); ++i) {
			const Vert &v = p_geo.verts[i];
			point[i] = Vector2(v.p.x, v.p.y);
			uv[i] = Vector2(v.uv.x, v.uv.y);
			color[i] = v.color;
		}
		std::copy(p_geo.indices.begin(), p_geo.indices.end(), indices.ptrw());
		rs->canvas_item_add_triangle_array(r_piece.item, indices, points, colors, uvs, PackedInt32Array(), PackedFloat32Array(), p_texture);
		r_piece.has_array = true;
		++g_counters.live_triangle_arrays;
		const uint64_t triangles = uint64_t(p_geo.indices.size() / 3);
		g_counters.smallest_triangle_array = g_counters.smallest_triangle_array == 0
				? triangles
				: std::min(g_counters.smallest_triangle_array, triangles);
	}

	void emit(Piece &r_piece, const Geometry &p_geo, RID p_texture) {
		rs->canvas_item_clear(r_piece.item);
		g_counters.live_triangle_arrays -= r_piece.has_array ? 1u : 0u;
		r_piece.has_array = false;
		r_piece.has_bounds = false;
		if (p_geo.indices.empty()) {
			return;
		}
		ui::SwfBounds b;
		for (const Vert &v : p_geo.verts) {
			b.add(v.p.x, v.p.y);
		}
		if (p_geo.indices.size() / 3 <= kPrimitiveMaxTriangles) {
			emit_primitives(r_piece, p_geo, p_texture);
		} else {
			emit_triangle_array(r_piece, p_geo, p_texture);
		}
		r_piece.bounds = ui::SwfRect{ float(b.x0), float(b.x1), float(b.y0), float(b.y1) };
		r_piece.has_bounds = !b.empty();
	}

	Piece &piece_at(NodeRec &r_rec, size_t p_index) {
		while (r_rec.pieces.size() <= p_index) {
			Piece p;
			p.item = make_item();
			++items_created;
			rs->canvas_item_set_material(p.item, item_material);
			rs->canvas_item_set_custom_rect(p.item, true, Rect2(-65536.0f, -65536.0f, 131072.0f, 131072.0f));
			r_rec.pieces.push_back(std::move(p));
		}
		return r_rec.pieces[p_index];
	}

	void configure(Piece &r_piece, int32_t p_mode, bool p_smooth, const float p_rect[4], const float p_inv[2]) {
		r_piece.mode = p_mode;
		r_piece.smooth = p_smooth;
		rs->canvas_item_set_default_texture_filter(r_piece.item,
				p_smooth ? RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR : RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_NEAREST);
		r_piece.uv = Color(p_rect[0], p_rect[1], p_rect[2], p_rect[3]);
		r_piece.inv = Vector2(p_inv[0], p_inv[1]);
		r_piece.applied.valid = false;
	}

	const ui::SwfSceneNode *grid_owner(const ui::SwfSceneNode &p_node, ui::SwfAffine *r_to_owner = nullptr) const {
		return ui::swf_grid_owner(*scene, p_node, r_to_owner);
	}

	ui::SwfGridWarp grid_for(const ui::SwfSceneNode &p_node) const {
		return ui::swf_grid_warp(*scene, p_node, stage_to_window);
	}

	double screen_scale(const ui::SwfSceneNode &p_node) const {
		const ui::SwfAffine w = ui::swf_concat(stage_to_window, ui::swf_affine(p_node.world));
		return std::sqrt(std::abs(w.a * w.d - w.b * w.c));
	}

	void build_shape(NodeRec &r_rec, const ui::SwfSceneNode &p_node, const ui::SwfShape &p_shape,
			const ui::SwfGridWarp &p_grid) {
		const ui::SwfShape &shape = p_shape;
		const ui::SwfGridWarp &grid = p_grid;
		r_rec.grid = grid.active;
		r_rec.built_scale_x = grid.sx;
		r_rec.built_scale_y = grid.sy;
		ui::SwfAffine to_sprite;
		if (grid.active) {
			grid_owner(p_node, &to_sprite);
		}
		r_rec.built_to_owner = to_sprite;
		auto pos = [&](ui::SwfPoint p) { return grid.active ? ui::swf_apply(to_sprite, p.x, p.y) : p; };
		size_t count = 0;
		Geometry solid;
		const float no_rect[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const float no_inv[2] = { 1.0f, 1.0f };
		auto flush_solid = [&]() {
			if (solid.indices.empty()) {
				return;
			}
			Piece &p = piece_at(r_rec, count++);
			configure(p, kModeSolid, true, no_rect, no_inv);
			p.texture = Ref<Texture2D>();
			emit(p, solid, RID());
			solid = Geometry();
		};
		for (const ui::SwfFillMesh &mesh : shape.meshes) {
			if (mesh.fill >= shape.fills.size()) {
				continue;
			}
			const ui::SwfFillStyle &fill = shape.fills[mesh.fill];
			if (fill.kind == ui::SwfFillKind::Solid) {
				const Color col = color_of(fill.color);
				for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
					Vert v[3];
					for (int k = 0; k < 3; ++k) {
						v[k].p = pos(mesh.vertices[mesh.indices[i + size_t(k)]]);
						v[k].color = col;
					}
					solid.add_warped(v[0], v[1], v[2], grid);
				}
				continue;
			}
			if (fill.kind != ui::SwfFillKind::Bitmap) {
				r_rec.counts.unsupported_fills++;
				warn_once(std::string("fill kind ") + ui::swf_fill_kind_name(fill.kind) + " in " + p_node.path);
				continue;
			}
			const std::optional<ui::SwfBitmapRef> ref = scene->bitmap(p_node, fill.bitmap_id);
			Ref<Texture2D> tex = ref ? atlas(p_node, ref->image_id) : Ref<Texture2D>();
			if (!ref || tex.is_null()) {
				r_rec.counts.missing_textures++;
				warn_once("no texture for bitmap " + std::to_string(fill.bitmap_id) + " in " + p_node.path);
				continue;
			}
			flush_solid();
			const ui::SwfAffine texel = ui::swf_inverse(ui::swf_affine(fill.texel_to_shape())).value_or(kSingularAffine);
			Geometry geo;
			for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
				Vert v[3];
				for (int k = 0; k < 3; ++k) {
					const ui::SwfPoint sp = mesh.vertices[mesh.indices[i + size_t(k)]];
					v[k].p = pos(sp);
					v[k].uv = ui::swf_apply(texel, sp.x, sp.y);
					v[k].color = Color(1.0f, 1.0f, 1.0f, 1.0f);
				}
				geo.add_warped(v[0], v[1], v[2], grid);
			}
			Piece &p = piece_at(r_rec, count++);
			const float rect[4] = { float(ref->x1), float(ref->y1), float(ref->x2 - ref->x1), float(ref->y2 - ref->y1) };
			const float inv[2] = { 1.0f / float(std::max<uint16_t>(1, ref->atlas_width)), 1.0f / float(std::max<uint16_t>(1, ref->atlas_height)) };
			configure(p, kModeBitmap, fill.smoothed(), rect, inv);
			p.wrap = fill.repeating();
			p.texture = tex;
			emit(p, geo, tex->get_rid());
		}
		r_rec.scale_dependent = false;
		for (const ui::SwfStroke &st : shape.strokes) {
			if (st.line >= shape.lines.size()) {
				continue;
			}
			const ui::SwfLineStyle &line = shape.lines[st.line];
			ui::SwfColor lc = line.color;
			if (line.has_fill) {
				if (line.fill.kind != ui::SwfFillKind::Solid) {
					r_rec.counts.unsupported_lines++;
					warn_once("non-solid line fill in " + p_node.path);
					continue;
				}
				lc = line.fill.color;
			}
			const double scale = std::max(1e-6, screen_scale(p_node));
			double hw = double(line.width) * 0.5;
			if (line.width <= 0.0f) {
				hw = 0.5 / scale;
				r_rec.scale_dependent = true;
			} else if (line.no_hscale || line.no_vscale) {
				hw = double(line.width) * 0.5 * std::sqrt(std::abs(stage_to_window.a * stage_to_window.d)) / scale;
				r_rec.scale_dependent = true;
			}
			r_rec.built_scale_x = scale;
			std::vector<ui::SwfPoint> outline;
			std::vector<uint32_t> ends;
			uint32_t begin = 0;
			for (size_t pth = 0; pth < st.path_ends.size(); ++pth) {
				const uint32_t end = std::min<uint32_t>(st.path_ends[pth], uint32_t(st.points.size()));
				if (end > begin) {
					const bool closed = pth < st.closed.size() && st.closed[pth];
					stroke_path(std::span<const ui::SwfPoint>(st.points.data() + begin, end - begin), closed, hw, line, outline, ends);
				}
				begin = end;
			}
			if (ends.empty()) {
				continue;
			}
			bf3::core::level::WorkBudget budget(kGeometryBudget);
			ui::SwfTriangulation tri;
			if (!ui::swf_triangulate_nonzero(outline, ends, budget, tri)) {
				r_rec.counts.unsupported_lines++;
				warn_once("stroke triangulation failed in " + p_node.path);
				continue;
			}
			const Color col = color_of(lc);
			for (size_t i = 0; i + 2 < tri.indices.size(); i += 3) {
				Vert v[3];
				for (int k = 0; k < 3; ++k) {
					v[k].p = pos(tri.vertices[tri.indices[i + size_t(k)]]);
					v[k].color = col;
				}
				solid.add_warped(v[0], v[1], v[2], grid);
			}
		}
		flush_solid();
		trim(r_rec, count);
	}

	void trim(NodeRec &r_rec, size_t p_count) {
		while (r_rec.pieces.size() > p_count) {
			free_piece(r_rec.pieces.back());
			r_rec.pieces.pop_back();
		}
	}

	void build_text(NodeRec &r_rec, const ui::SwfSceneNode &p_node) {
		Piece &p = piece_at(r_rec, 0);
		trim(r_rec, 1);
		const ui::SwfFontSet *fonts = assets != nullptr ? assets->swf_fonts() : nullptr;
		Geometry geo;
		const ui::SwfRect field = ui::swf_node_text_bounds(p_node);
		float rect[4] = { field.x_min, field.y_min, field.x_max, field.y_max };
		const float inv[2] = { 1.0f, 1.0f };
		if (fonts == nullptr) {
			r_rec.counts.missing_fonts++;
			warn_once("no DefineFont3 font set for " + p_node.path);
		} else {
			bf3::core::level::WorkBudget budget(ui::kSwfTextBudgetUnits);
			bf3::core::Result<ui::SwfTextLayout> layout = ui::swf_layout_node_text(*fonts, p_node, budget);
			if (!layout) {
				warn_once(layout.error().message);
			} else {
				rect[0] = layout->bounds.x_min;
				rect[1] = layout->bounds.y_min;
				rect[2] = layout->bounds.x_max;
				rect[3] = layout->bounds.y_max;
				r_rec.counts.missing_fonts += layout->missing_fonts;
				r_rec.counts.missing_glyphs += layout->missing_glyphs;
				for (const ui::SwfGlyphPlacement &g : layout->glyphs) {
					if (g.face == nullptr) {
						continue;
					}
					const GlyphMesh &mesh = glyph_mesh(g.face, g.glyph);
					if (!mesh.ok) {
						r_rec.counts.missing_glyphs++;
						continue;
					}
					if (g.underline) {
						warn_once("underline not drawn in " + p_node.path);
					}
					const double s = double(g.size) / double(ui::kSwfFontUnitsPerEm);
					const Color col = color_of(g.color);
					const int32_t base = int32_t(geo.verts.size());
					for (const ui::SwfPoint &v : mesh.vertices) {
						Vert out;
						out.p = { float(g.x + v.x * s), float(g.baseline + v.y * s) };
						out.uv = out.p;
						out.color = col;
						geo.verts.push_back(out);
					}
					for (const uint32_t i : mesh.indices) {
						geo.indices.push_back(base + int32_t(i));
					}
					r_rec.counts.glyphs++;
				}
			}
		}
		if (p_node.edit_text->has(ui::SwfEditText::kBorder)) {
			warn_once("edit text border not drawn in " + p_node.path);
		}
		configure(p, kModeClipped, true, rect, inv);
		p.texture = Ref<Texture2D>();
		emit(p, geo, RID());
		if (p.has_bounds) {
			p.bounds.x_min = std::max(p.bounds.x_min, rect[0]);
			p.bounds.y_min = std::max(p.bounds.y_min, rect[1]);
			p.bounds.x_max = std::min(p.bounds.x_max, rect[2]);
			p.bounds.y_max = std::min(p.bounds.y_max, rect[3]);
			p.has_bounds = p.bounds.x_min < p.bounds.x_max && p.bounds.y_min < p.bounds.y_max;
		}
	}

	void build_bitmap(NodeRec &r_rec, const ui::SwfSceneNode &p_node) {
		const ui::SwfSubImage &sub = *p_node.sub_image;
		const ui::SwfExternalImage *image = p_node.movie->find_external_image(sub.image_id);
		Ref<Texture2D> tex = atlas(p_node, sub.image_id);
		if (image == nullptr || tex.is_null()) {
			r_rec.counts.missing_textures++;
			warn_once("no texture for bitmap node " + p_node.path);
			trim(r_rec, 0);
			return;
		}
		Piece &p = piece_at(r_rec, 0);
		trim(r_rec, 1);
		const float w = float(sub.width());
		const float h = float(sub.height());
		Geometry geo;
		const Color white(1.0f, 1.0f, 1.0f, 1.0f);
		const Vert a{ { 0.0f, 0.0f }, { 0.0f, 0.0f }, white };
		const Vert b{ { w, 0.0f }, { w, 0.0f }, white };
		const Vert c{ { w, h }, { w, h }, white };
		const Vert d{ { 0.0f, h }, { 0.0f, h }, white };
		geo.add_triangle(a, b, c);
		geo.add_triangle(a, c, d);
		const float rect[4] = { float(sub.x1), float(sub.y1), w, h };
		const float inv[2] = { 1.0f / float(std::max<uint16_t>(1, image->width)), 1.0f / float(std::max<uint16_t>(1, image->height)) };
		configure(p, kModeBitmap, true, rect, inv);
		p.wrap = false;
		p.texture = tex;
		emit(p, geo, tex->get_rid());
	}

	void build_image(NodeRec &r_rec, const ui::SwfSceneNode &p_node) {
		Ref<Texture2D> tex = textures != nullptr ? textures->texture(p_node.image) : Ref<Texture2D>();
		if (tex.is_null()) {
			r_rec.counts.missing_textures++;
			warn_once("no texture for image " + p_node.image + " in " + p_node.path);
			trim(r_rec, 0);
			return;
		}
		Piece &p = piece_at(r_rec, 0);
		trim(r_rec, 1);
		const float w = float(p_node.image_width);
		const float h = float(p_node.image_height);
		const float tw = float(std::max<int32_t>(1, tex->get_width()));
		const float th = float(std::max<int32_t>(1, tex->get_height()));
		Geometry geo;
		const Color white(1.0f, 1.0f, 1.0f, 1.0f);
		const Vert a{ { 0.0f, 0.0f }, { 0.0f, 0.0f }, white };
		const Vert b{ { w, 0.0f }, { tw, 0.0f }, white };
		const Vert c{ { w, h }, { tw, th }, white };
		const Vert d{ { 0.0f, h }, { 0.0f, th }, white };
		geo.add_triangle(a, b, c);
		geo.add_triangle(a, c, d);
		const float rect[4] = { 0.0f, 0.0f, tw, th };
		const float inv[2] = { 1.0f / tw, 1.0f / th };
		configure(p, kModeBitmap, true, rect, inv);
		p.wrap = false;
		p.texture = tex;
		emit(p, geo, tex->get_rid());
	}

	void build(ui::SwfNodeId p_id, NodeRec &r_rec) {
		const ui::SwfSceneNode &n = *node(p_id);
		const void *content = n.shape != nullptr ? static_cast<const void *>(n.shape)
				: n.edit_text != nullptr		 ? static_cast<const void *>(n.edit_text)
				: n.sub_image != nullptr		 ? static_cast<const void *>(n.sub_image)
				: n.drawing != nullptr			 ? static_cast<const void *>(n.drawing.get())
				: !n.image.empty()				 ? static_cast<const void *>(&n.image)
												 : nullptr;
		const bool reopened = r_rec.reopened;
		r_rec.reopened = false;
		if (r_rec.built && !reopened && r_rec.content == content && r_rec.text_revision == n.text_revision &&
				r_rec.character_revision == n.character_revision) {
			return;
		}
		const uint64_t fingerprint = content_fingerprint(n);
		r_rec.content = content;
		r_rec.text_revision = n.text_revision;
		r_rec.character_revision = n.character_revision;
		if (r_rec.built && reopened && r_rec.fingerprint == fingerprint) {
			return;
		}
		const auto build_start = std::chrono::steady_clock::now();
		stats.content_builds++;
		r_rec.built = true;
		r_rec.fingerprint = fingerprint;
		r_rec.content_dirty = true;
		r_rec.counts = BuildCounts();
		if (n.kind == ui::SwfNodeKind::Shape && n.shape != nullptr) {
			build_shape(r_rec, n, *n.shape, grid_for(n));
		} else if (!n.image.empty()) {
			build_image(r_rec, n);
		} else if (n.drawing != nullptr) {
			build_shape(r_rec, n, *n.drawing, ui::SwfGridWarp{});
		} else if (n.kind == ui::SwfNodeKind::EditText && n.edit_text != nullptr) {
			build_text(r_rec, n);
		} else if (n.kind == ui::SwfNodeKind::Bitmap && n.sub_image != nullptr) {
			build_bitmap(r_rec, n);
		} else {
			if (n.kind == ui::SwfNodeKind::StaticText) {
				warn_once("static text not drawn: " + n.path);
			}
			trim(r_rec, 0);
		}
		for (Piece &p : r_rec.pieces) {
			p.applied.valid = false;
		}
		g_counters.build_seconds += bf3::core::seconds_since(build_start);
	}

	uint32_t drawables(ui::SwfNodeId p_id) {
		const ui::SwfSceneNode *n = node(p_id);
		if (n == nullptr) {
			return 0;
		}
		NodeRec &rec = rec_for(p_id);
		build(p_id, rec);
		uint32_t count = 0;
		for (const Piece &p : rec.pieces) {
			count += p.has_bounds ? 1 : 0;
		}
		for (const ui::SwfNodeId c : n->children) {
			if (count >= 2) {
				break;
			}
			const ui::SwfSceneNode *k = node(c);
			if (k != nullptr && k->clip_depth == 0 && !k->is_mask) {
				count += drawables(c);
			}
		}
		return count;
	}

	Canvas &canvas_for(CanvasKind p_kind, ui::SwfNodeId p_owner, Canvas *p_parent) {
		const uint64_t key = canvas_key(p_kind, p_owner);
		const ui::SwfSceneNode *owner_node = p_kind == CanvasKind::Root ? nullptr : node(p_owner);
		const uint64_t owner_path = owner_node != nullptr ? path_hash(*owner_node) : 0;
		if (owner_path != 0 && !dormant_canvases.empty() && canvases.find(key) == canvases.end()) {
			const auto d = dormant_canvases.find(dormant_key(p_kind, owner_path));
			if (d != dormant_canvases.end()) {
				auto handle = dormant_canvases.extract(d);
				handle.key() = key;
				canvases.insert(std::move(handle));
			} else {
				recycle_dormant(p_kind);
			}
		}
		Canvas &c = canvases[key];
		c.owner_path = owner_path;
		const bool fresh = !c.content.canvas.is_valid();
		c.changed = fresh || c.parent != p_parent || c.kind != p_kind;
		c.kind = p_kind;
		c.owner = p_owner;
		c.parent = p_parent;
		c.used = true;
		c.next_index = 0;
		c.next_alpha_index = 0;
		c.old_nodes.swap(c.nodes);
		c.old_alpha_nodes.swap(c.alpha_nodes);
		c.old_children.swap(c.children);
		c.old_targets.swap(c.targets);
		c.nodes.clear();
		c.alpha_nodes.clear();
		c.children.clear();
		c.targets.clear();
		if (fresh) {
			c.render_dirty = true;
			c.bounds_dirty = true;
		}
		if (p_parent != nullptr) {
			p_parent->children.push_back(&c);
		}
		if (fresh) {
			if (p_kind == CanvasKind::Root) {
				c.content.create(rs, target_size, owner_viewport, msaa);
			} else {
				c.content.create_canvas_only(rs);
				if (p_kind == CanvasKind::Mask) {
					c.alpha.create_canvas_only(rs);
				}
				c.quad = make_item();
				++items_created;
				if (p_kind == CanvasKind::Mask) {
					c.quad_material = rs->material_create();
					rs->material_set_shader(c.quad_material, item_shader);
					rs->material_set_param(c.quad_material, "mask_tex", c.alpha.texture);
					rs->canvas_item_set_material(c.quad, c.quad_material);
				} else {
					rs->canvas_item_set_material(c.quad, item_material);
				}
				rs->canvas_item_set_custom_rect(c.quad, true, Rect2(-65536.0f, -65536.0f, 131072.0f, 131072.0f));
				c.quad_applied = Applied();
				c.quad_parent = RID();
				c.quad_index = -1;
			}
		}
		if (p_kind == CanvasKind::Root) {
			c.tilt_floor = ui::kSwfNoNode;
			c.color_floor = ui::kSwfNoNode;
		} else if (p_kind == CanvasKind::Filter) {
			c.tilt_floor = p_owner;
			c.color_floor = p_owner;
		} else if (p_kind == CanvasKind::Blend) {
			c.tilt_floor = p_parent->tilt_floor;
			c.color_floor = p_owner;
		} else {
			c.tilt_floor = p_parent->tilt_floor;
			c.color_floor = p_parent->color_floor;
		}
		order.push_back(&c);
		return c;
	}

	void place_quad(Canvas &r_c) {
		Canvas &p = *r_c.parent;
		const int32_t index = p.next_index++;
		if (r_c.quad_parent != p.content.canvas) {
			rs->canvas_item_set_parent(r_c.quad, p.content.canvas);
			r_c.quad_parent = p.content.canvas;
			p.changed = true;
		}
		if (r_c.quad_index != index) {
			rs->canvas_item_set_draw_index(r_c.quad, index);
			r_c.quad_index = index;
			p.changed = true;
		}
	}

	void setup_filter_stages(Canvas &r_c, const std::vector<ui::SwfFilter> &p_filters) {
		const bool same = r_c.stages.size() == p_filters.size() && r_c.filters.size() == p_filters.size() &&
				std::equal(p_filters.begin(), p_filters.end(), r_c.filters.begin(), [](const ui::SwfFilter &p_a, const ui::SwfFilter &p_b) {
					return p_a.kind == p_b.kind && p_a.color == p_b.color && p_a.blur_x == p_b.blur_x && p_a.blur_y == p_b.blur_y && p_a.angle == p_b.angle &&
							p_a.distance == p_b.distance && p_a.strength == p_b.strength && p_a.inner == p_b.inner && p_a.knockout == p_b.knockout &&
							p_a.composite_source == p_b.composite_source && p_a.passes == p_b.passes;
				});
		if (same) {
			return;
		}
		for (Stage &s : r_c.stages) {
			free_item(s.item);
			release_filter_material(rs, s.material_key);
			retire(s.surface, RenderingServer::VIEWPORT_MSAA_DISABLED);
		}
		r_c.stages.clear();
		r_c.linked_stages = size_t(-1);
		r_c.quad_texture = RID();
		r_c.changed = true;
		r_c.filters = p_filters;
		r_c.margin_x = 0.0;
		r_c.margin_y = 0.0;
		const double kx = fit.scale_x;
		const double ky = fit.scale_y;
		for (const ui::SwfFilter &f : p_filters) {
			Stage s;
			s.surface.create_canvas_only(rs);
			s.item = make_item();
			rs->canvas_item_set_parent(s.item, s.surface.canvas);
			rs->canvas_item_set_default_texture_filter(s.item, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
			const ui::SwfFilterKernel kernel = ui::swf_filter_kernel(f, kx, ky);
			stats.kernels_clamped += kernel.clamped;
			s.material_key = filter_material_key(f, kernel, kx, ky);
			rs->canvas_item_set_material(s.item, acquire_filter_material(rs, filter_shader, s.material_key, f, kernel));
			r_c.margin_x += kernel.margin_x;
			r_c.margin_y += kernel.margin_y;
			r_c.stages.push_back(std::move(s));
		}
	}

	void link_viewport(RID p_child, RID p_parent) {
		g_counters.viewport_links_without_viewport += p_child.is_valid() ? 0u : 1u;
		rs->viewport_set_parent_viewport(p_child, p_parent);
	}

	void link_viewports(Canvas &r_c) {
		RID parent = r_c.parent != nullptr ? r_c.parent->content.viewport : owner_viewport;
		const bool stages_ready = std::all_of(
				r_c.stages.begin(), r_c.stages.end(), [](const Stage &p_s) { return p_s.surface.viewport.is_valid(); });
		if (!r_c.content.viewport.is_valid() || !parent.is_valid() || !stages_ready) {
			return;
		}
		if (r_c.linked_parent == parent && r_c.linked_stages == r_c.stages.size()) {
			return;
		}
		r_c.linked_parent = parent;
		r_c.linked_stages = r_c.stages.size();
		if (r_c.kind == CanvasKind::Filter && !r_c.stages.empty()) {
			link_viewport(r_c.content.viewport, r_c.stages.front().surface.viewport);
			for (size_t i = 0; i + 1 < r_c.stages.size(); ++i) {
				link_viewport(r_c.stages[i].surface.viewport, r_c.stages[i + 1].surface.viewport);
			}
			link_viewport(r_c.stages.back().surface.viewport, parent);
		} else {
			link_viewport(r_c.content.viewport, parent);
		}
		if (r_c.alpha.viewport.is_valid()) {
			link_viewport(r_c.alpha.viewport, parent);
		}
	}

	void attach_pieces(ui::SwfNodeId p_id, NodeRec &r_rec, Canvas &r_c, bool p_alpha, bool p_additive) {
		r_rec.canvas = &r_c;
		r_rec.in_alpha = p_alpha;
		r_rec.drawn = true;
		r_rec.additive = p_additive;
		const RID target = p_alpha ? r_c.alpha.canvas : r_c.content.canvas;
		(p_alpha ? r_c.alpha_nodes : r_c.nodes).push_back(p_id);
		for (Piece &p : r_rec.pieces) {
			const int32_t index = p_alpha ? r_c.next_alpha_index++ : r_c.next_index++;
			if (p.parent != target) {
				rs->canvas_item_set_parent(p.item, target);
				p.parent = target;
				r_c.changed = true;
			}
			if (p.index != index) {
				rs->canvas_item_set_draw_index(p.item, index);
				p.index = index;
				r_c.changed = true;
			}
			p.additive = p_additive;
		}
	}

	struct Ctx {
		bool additive = false;
		bool alpha = false;
		bool enabled = true;
	};

	void plan_node(ui::SwfNodeId p_id, Canvas &r_canvas, Ctx p_ctx) {
		const ui::SwfSceneNode *n = node(p_id);
		if (n == nullptr) {
			return;
		}
		NodeRec &rec = rec_for(p_id);
		rec.seen = true;
		rec.layer = nullptr;
		bool own = true;
		if (!p_ctx.enabled) {
			if (p_id == isolated_id) {
				p_ctx.enabled = true;
			} else if (isolated_ancestors.count(p_id) != 0) {
				own = false;
			} else {
				return;
			}
		}
		if (!p_ctx.alpha && n->is_mask && n->clip_depth == 0) {
			return;
		}
		const ui::SwfSceneNode *script_mask = !p_ctx.alpha && own ? node(n->script_mask) : nullptr;
		Canvas *canvas = &r_canvas;
		if (script_mask != nullptr) {
			Canvas &g = canvas_for(CanvasKind::Mask, script_mask->id, &r_canvas);
			link_viewports(g);
			place_quad(g);
			stats.mask_groups++;
			canvas = &g;
		}
		if (!p_ctx.alpha && own) {
			const auto rt = renderers.find(p_id);
			if (rt != renderers.end() && rt->second.find(";i") != std::string::npos) {
				plan_target(p_id, rt->second, *canvas);
				plan_script_mask(script_mask, *canvas, p_ctx);
				return;
			}
			std::vector<ui::SwfFilter> filters;
			for (const ui::SwfFilter &f : n->filters) {
				if (ui::swf_filter_supported(f)) {
					filters.push_back(f);
				} else {
					stats.unsupported_filters++;
					warn_once(std::string("filter ") + ui::swf_filter_kind_name(f.kind) + " not drawn on " + n->path);
				}
			}
			const uint8_t blend = n->blend_mode;
			const bool add = blend == ui::swf_blend::kAdd;
			const bool layer = blend == ui::swf_blend::kLayer;
			if (blend > ui::swf_blend::kLayer && !add) {
				stats.unsupported_blends++;
				warn_once("blend mode " + std::to_string(blend) + " drawn as normal on " + n->path);
			}
			if (filters_enabled && !filters.empty()) {
				Canvas &f = canvas_for(CanvasKind::Filter, p_id, canvas);
				f.additive = add || p_ctx.additive;
				setup_filter_stages(f, filters);
				link_viewports(f);
				place_quad(f);
				rec.layer = &f;
				stats.filter_layers++;
				stats.filter_passes += uint32_t(filters.size());
				for (const ui::SwfFilter &fl : filters) {
					(fl.kind == ui::SwfFilterKind::DropShadow ? stats.drop_shadow_passes
									: fl.kind == ui::SwfFilterKind::Glow ? stats.glow_passes
																		  : stats.blur_passes)++;
				}
				canvas = &f;
				p_ctx.additive = false;
			} else if (add || layer) {
				if (drawables(p_id) >= 2) {
					Canvas &b = canvas_for(CanvasKind::Blend, p_id, canvas);
					b.additive = add || p_ctx.additive;
					link_viewports(b);
					place_quad(b);
					rec.layer = &b;
					stats.blend_layers++;
					canvas = &b;
					p_ctx.additive = false;
				} else if (add) {
					p_ctx.additive = true;
				}
			}
		}
		build(p_id, rec);
		if (own) {
			attach_pieces(p_id, rec, *canvas, p_ctx.alpha, p_ctx.additive);
		}
		plan_range(n->children, 0, n->children.size(), *canvas, p_ctx);
		if (script_mask != nullptr) {
			plan_script_mask(script_mask, canvases[canvas_key(CanvasKind::Mask, script_mask->id)], p_ctx);
		}
	}

	void plan_script_mask(const ui::SwfSceneNode *p_mask, Canvas &r_group, Ctx p_ctx) {
		if (p_mask == nullptr) {
			return;
		}
		p_ctx.alpha = true;
		p_ctx.additive = false;
		p_ctx.enabled = true;
		plan_node(p_mask->id, r_group, p_ctx);
	}

	void plan_range(const std::vector<ui::SwfNodeId> &p_kids, size_t p_begin, size_t p_end, Canvas &r_canvas, Ctx p_ctx) {
		for (size_t i = p_begin; i < p_end; ++i) {
			const ui::SwfSceneNode *c = node(p_kids[i]);
			if (c == nullptr) {
				continue;
			}
			if (c->clip_depth == 0) {
				plan_node(p_kids[i], r_canvas, p_ctx);
				continue;
			}
			size_t j = i + 1;
			while (j < p_end) {
				const ui::SwfSceneNode *k = node(p_kids[j]);
				if (k == nullptr || k->depth > int32_t(c->clip_depth)) {
					break;
				}
				++j;
			}
			if (p_ctx.alpha) {
				rec_for(p_kids[i]).seen = true;
				plan_range(p_kids, i + 1, j, r_canvas, p_ctx);
				i = j - 1;
				continue;
			}
			Canvas &g = canvas_for(CanvasKind::Mask, p_kids[i], &r_canvas);
			link_viewports(g);
			place_quad(g);
			stats.mask_groups++;
			Ctx mask_ctx = p_ctx;
			mask_ctx.alpha = true;
			mask_ctx.additive = false;
			mask_ctx.enabled = true;
			plan_node(p_kids[i], g, mask_ctx);
			plan_range(p_kids, i + 1, j, g, p_ctx);
			i = j - 1;
		}
	}

	void plan_target(ui::SwfNodeId p_id, const std::string &p_renderer, Canvas &r_canvas) {
		Target &t = targets[p_id];
		const ui::SwfSceneNode &n = *node(p_id);
		t.node = p_id;
		t.renderer = p_renderer;
		t.path = n.path;
		t.used = true;
		t.canvas = &r_canvas;
		r_canvas.targets.push_back(p_id);
		ui::SwfBounds local;
		const ui::SwfAffine to_local = ui::swf_inverse(ui::swf_affine(n.world)).value_or(kSingularAffine);
		std::vector<ui::SwfNodeId> stack(n.children.begin(), n.children.end());
		while (!stack.empty()) {
			const ui::SwfNodeId id = stack.back();
			stack.pop_back();
			const ui::SwfSceneNode *k = node(id);
			if (k == nullptr) {
				continue;
			}
			NodeRec &rec = rec_for(id);
			rec.seen = true;
			build(id, rec);
			const ui::SwfAffine m = ui::swf_concat(to_local, ui::swf_affine(k->world));
			for (const Piece &p : rec.pieces) {
				if (p.has_bounds) {
					local.add(project_rect(p.bounds, m, Hom{}));
				}
			}
			stack.insert(stack.end(), k->children.begin(), k->children.end());
		}
		if (local.empty()) {
			local.add(0.0, 0.0);
			local.add(1.0, 1.0);
		}
		t.local = local.rect();
		const ui::SwfAffine w = ui::swf_concat(stage_to_window, ui::swf_affine(n.world));
		const double sx = std::sqrt(w.a * w.a + w.b * w.b);
		const double sy = std::sqrt(w.c * w.c + w.d * w.d);
		const Vector2i size(std::clamp(int32_t(std::ceil(t.local.width() * sx)), 1, kMaxLayerSide), std::clamp(int32_t(std::ceil(t.local.height() * sy)), 1, kMaxLayerSide));
		if (!t.surface.viewport.is_valid()) {
			t.surface.create(rs, size, r_canvas.content.viewport, msaa);
			t.root_item = make_item();
			rs->canvas_item_set_parent(t.root_item, t.surface.canvas);
			t.quad = make_item();
			++items_created;
			rs->canvas_item_set_material(t.quad, item_material);
			rs->canvas_item_set_custom_rect(t.quad, true, Rect2(-65536.0f, -65536.0f, 131072.0f, 131072.0f));
			rs->canvas_item_set_default_texture_filter(t.quad, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
			t.redraw = true;
		} else {
			t.surface.resize(rs, size);
			rs->viewport_set_parent_viewport(t.surface.viewport, r_canvas.content.viewport);
		}
		if (!(t.drawn_local == t.local) || t.drawn_size != size) {
			t.drawn_local = t.local;
			t.drawn_size = size;
			rs->canvas_item_set_transform(t.root_item,
					Transform2D(real_t(size.x / std::max(1e-6f, t.local.width())), 0.0f, 0.0f, real_t(size.y / std::max(1e-6f, t.local.height())), 0.0f, 0.0f) *
							Transform2D(1.0f, 0.0f, 0.0f, 1.0f, -t.local.x_min, -t.local.y_min));
			rs->canvas_item_clear(t.quad);
			rs->canvas_item_add_texture_rect(t.quad, Rect2(t.local.x_min, t.local.y_min, t.local.width(), t.local.height()), t.surface.texture);
			t.redraw = true;
			r_canvas.changed = true;
		}
		const int32_t index = r_canvas.next_index++;
		if (t.quad_parent != r_canvas.content.canvas) {
			rs->canvas_item_set_parent(t.quad, r_canvas.content.canvas);
			t.quad_parent = r_canvas.content.canvas;
			r_canvas.changed = true;
		}
		if (t.quad_index != index) {
			rs->canvas_item_set_draw_index(t.quad, index);
			t.quad_index = index;
			r_canvas.changed = true;
		}
		stats.engine_targets++;
	}

	void resolve_paths() {
		tilts.clear();
		for (const auto &[path, tilt] : tilt_paths) {
			const ui::SwfNodeId id = scene->find(path);
			if (id != ui::kSwfNoNode) {
				tilts[id] = tilt;
			}
		}
		renderers.clear();
		for (const auto &[path, r] : renderer_paths) {
			const ui::SwfNodeId id = scene->find(path);
			if (id != ui::kSwfNoNode) {
				renderers[id] = r;
			}
		}
		isolated_id = ui::kSwfNoNode;
		isolated_ancestors.clear();
		if (!isolated.empty()) {
			isolated_id = scene->find(isolated);
			for (const ui::SwfSceneNode *a = isolated_id != ui::kSwfNoNode ? node(node(isolated_id)->parent) : nullptr; a != nullptr; a = node(a->parent)) {
				isolated_ancestors.insert(a->id);
			}
		}
	}

	void plan() {
		const auto plan_start = std::chrono::steady_clock::now();
		stats.structure_passes++;
		const SwfViewStats keep = stats;
		stats = SwfViewStats();
		stats.structure_passes = keep.structure_passes;
		stats.content_builds = keep.content_builds;
		stats.reopens = keep.reopens;
		resolve_paths();
		for (auto &[k, c] : canvases) {
			c.used = false;
		}
		for (auto &[id, t] : targets) {
			t.used = false;
		}
		for (auto &[id, rec] : recs) {
			rec.seen = false;
			rec.drawn = false;
			rec.canvas = nullptr;
			rec.layer = nullptr;
		}
		order.clear();
		Canvas &root = canvas_for(CanvasKind::Root, 0, nullptr);
		if (root.content.resize(rs, target_size)) {
			root.changed = true;
		}
		rs->viewport_set_parent_viewport(root.content.viewport, owner_viewport);
		root.origin = Vector2i();
		root.size = target_size;
		Ctx ctx;
		ctx.enabled = isolated.empty();
		plan_node(scene->root(), root, ctx);
		for (auto it = recs.begin(); it != recs.end();) {
			if (node(it->first) == nullptr) {
				park_rec(std::move(it->second));
				it = recs.erase(it);
				continue;
			}
			if (!it->second.drawn) {
				for (Piece &p : it->second.pieces) {
					rs->canvas_item_set_visible(p.item, false);
					p.applied.visible = false;
				}
			}
			++it;
		}
		for (auto it = targets.begin(); it != targets.end();) {
			if (!it->second.used) {
				free_target(it->second);
				it = targets.erase(it);
			} else {
				++it;
			}
		}
		for (auto it = canvases.begin(); it != canvases.end();) {
			it = it->second.used ? std::next(it) : park_canvas(it);
		}
		trim_dormant();
		hide_dormant();
		for (Canvas *c : order) {
			if (c->changed || !same_ids(c->nodes, c->old_nodes) || !same_ids(c->alpha_nodes, c->old_alpha_nodes) ||
					c->children != c->old_children || !same_ids(c->targets, c->old_targets)) {
				mark(c);
			}
			c->changed = false;
		}
		adopted_ids.clear();
		for (auto &[id, rec] : recs) {
			if (!rec.drawn) {
				continue;
			}
			const ui::SwfSceneNode *n = node(id);
			stats.glyphs += rec.counts.glyphs;
			stats.missing_glyphs += rec.counts.missing_glyphs;
			stats.missing_fonts += rec.counts.missing_fonts;
			stats.missing_textures += rec.counts.missing_textures;
			stats.unsupported_fills += rec.counts.unsupported_fills;
			stats.unsupported_lines += rec.counts.unsupported_lines;
			stats.tilted_items += tilt_chain(id, rec.canvas->tilt_floor).identity ? 0 : uint32_t(rec.pieces.size());
			for (const Piece &p : rec.pieces) {
				stats.items++;
				stats.additive_items += p.additive ? 1 : 0;
				if (p.mode == kModeClipped) {
					stats.text_items++;
				} else if (p.mode == kModeBitmap) {
					(n->kind == ui::SwfNodeKind::Bitmap ? stats.bitmap_items : stats.bitmap_fill_items)++;
				} else {
					stats.fill_items++;
				}
				stats.scaling_grid_items += rec.grid ? 1 : 0;
			}
			if (n->shape != nullptr) {
				stats.stroke_items += uint32_t(n->shape->strokes.size());
			}
		}
		structure_dirty = false;
		++g_counters.plans;
		g_counters.plan_seconds += bf3::core::seconds_since(plan_start);
	}

	bool apply_piece(Piece &r_p, const Transform2D &p_xf, const std::array<Vector3, 3> &p_h, const Color &p_mul, const Color &p_add, int32_t p_flags, bool p_visible) {
		Applied &a = r_p.applied;
		const bool same = a.valid && a.visible == p_visible && a.xform == p_xf && a.h == p_h && a.mul == p_mul && a.add == p_add && a.flags == p_flags &&
				a.mode == r_p.mode && a.uv == r_p.uv && a.inv == r_p.inv;
		if (same) {
			return false;
		}
		if (!a.valid || a.visible != p_visible) {
			rs->canvas_item_set_visible(r_p.item, p_visible);
		}
		if (!a.valid || a.xform != p_xf) {
			rs->canvas_item_set_transform(r_p.item, p_xf);
		}
		if (!a.valid || a.h != p_h) {
			rs->canvas_item_set_instance_shader_parameter(r_p.item, "h0", p_h[0]);
			rs->canvas_item_set_instance_shader_parameter(r_p.item, "h1", p_h[1]);
			rs->canvas_item_set_instance_shader_parameter(r_p.item, "h2", p_h[2]);
		}
		if (!a.valid || a.mul != p_mul || a.add != p_add) {
			rs->canvas_item_set_instance_shader_parameter(r_p.item, "cx_mul", vec4_of(p_mul));
			rs->canvas_item_set_instance_shader_parameter(r_p.item, "cx_add", vec4_of(p_add));
		}
		if (!a.valid || a.flags != p_flags) {
			rs->canvas_item_set_instance_shader_parameter(r_p.item, "flags", p_flags);
		}
		if (!a.valid || a.mode != r_p.mode || a.uv != r_p.uv || a.inv != r_p.inv) {
			rs->canvas_item_set_instance_shader_parameter(r_p.item, "mode", r_p.mode);
			rs->canvas_item_set_instance_shader_parameter(r_p.item, "uv_rect", vec4_of(r_p.uv));
			rs->canvas_item_set_instance_shader_parameter(r_p.item, "atlas_inv", r_p.inv);
		}
		a.mode = r_p.mode;
		a.uv = r_p.uv;
		a.inv = r_p.inv;
		a.valid = true;
		a.visible = p_visible;
		a.xform = p_xf;
		a.h = p_h;
		a.mul = p_mul;
		a.add = p_add;
		a.flags = p_flags;
		return true;
	}

	static std::array<Vector3, 3> rows(const Hom &p_h) {
		return { Vector3(real_t(p_h.m[0]), real_t(p_h.m[1]), real_t(p_h.m[2])), Vector3(real_t(p_h.m[3]), real_t(p_h.m[4]), real_t(p_h.m[5])),
			Vector3(real_t(p_h.m[6]), real_t(p_h.m[7]), real_t(p_h.m[8])) };
	}

	bool apply_quad(RID p_item, Applied &r_a, const Transform2D &p_xf, const Hom &p_h, const ui::SwfColorXform &p_cx, int32_t p_flags, bool p_visible) {
		Piece tmp;
		tmp.item = p_item;
		tmp.mode = kModeLayer;
		tmp.applied = r_a;
		const bool changed = apply_piece(tmp, p_xf, rows(p_h), shader_mul(p_cx), shader_add(p_cx), p_flags, p_visible);
		r_a = tmp.applied;
		return changed;
	}

	void mark(Canvas *p_c) {
		for (Canvas *c = p_c; c != nullptr; c = c->parent) {
			c->render_dirty = true;
			c->bounds_dirty = true;
		}
	}

	void refresh(ui::SwfNodeId p_id) {
		const auto it = recs.find(p_id);
		if (it == recs.end()) {
			return;
		}
		NodeRec &rec = it->second;
		const ui::SwfSceneNode *n = node(p_id);
		if (n == nullptr) {
			return;
		}
		stats.nodes_refreshed++;
		if (rec.drawn && rec.canvas != nullptr) {
			const bool rebuild_grid = rec.grid && [&] {
				const ui::SwfGridWarp g = grid_for(*n);
				ui::SwfAffine to_owner;
				grid_owner(*n, &to_owner);
				return !g.active || to_owner != rec.built_to_owner || std::abs(g.sx - rec.built_scale_x) > 1e-4 ||
						std::abs(g.sy - rec.built_scale_y) > 1e-4;
			}();
			const bool rebuild_stroke = rec.scale_dependent && std::abs(screen_scale(*n) - rec.built_scale_x) > 1e-4 * std::max(1.0, rec.built_scale_x);
			if (rebuild_grid || rebuild_stroke) {
				rec.built = false;
				build(p_id, rec);
				structure_dirty = true;
				return;
			}
			Canvas &c = *rec.canvas;
			const ui::SwfSceneNode *grid_root = rec.grid ? grid_owner(*n) : nullptr;
			const ui::SwfSceneNode *frame = grid_root != nullptr ? grid_root : n;
			const Transform2D xf = xform_of(ui::swf_concat(stage_to_window, ui::swf_affine(frame->world)));
			const std::array<Vector3, 3> h = rows(tilt_chain(p_id, c.tilt_floor));
			Color cmul(0.0f, 0.0f, 0.0f, 0.0f);
			Color cadd(1.0f, 1.0f, 1.0f, 1.0f);
			if (!rec.in_alpha) {
				const ui::SwfColorXform cx = relative_color(p_id, c.color_floor);
				cmul = shader_mul(cx);
				cadd = shader_add(cx);
			}
			const bool visible = rec.in_alpha || n->world_visible;
			bool changed = false;
			for (Piece &p : rec.pieces) {
				const int32_t flags = (p.wrap ? kFlagWrap : 0) | (p.additive ? kFlagAdditive : 0);
				changed = apply_piece(p, xf, h, cmul, cadd, flags, visible) || changed;
			}
			if (changed || rec.content_dirty) {
				rec.content_dirty = false;
				mark(&c);
			}
		}
		if (rec.layer != nullptr && rec.layer->parent != nullptr) {
			Canvas &l = *rec.layer;
			Canvas &p = *l.parent;
			const Hom h = l.kind == CanvasKind::Filter ? tilt_chain(p_id, p.tilt_floor) : Hom{};
			const ui::SwfColorXform cx = l.kind == CanvasKind::Filter || l.kind == CanvasKind::Blend ? relative_color(p_id, p.color_floor) : ui::SwfColorXform{};
			const int32_t flags = l.additive ? kFlagAdditive : 0;
			if (apply_quad(l.quad, l.quad_applied, Transform2D(), h, cx, flags, n->world_visible && l.has_content)) {
				mark(&p);
			}
		}
		const auto tt = targets.find(p_id);
		if (tt != targets.end() && tt->second.canvas != nullptr) {
			Target &t = tt->second;
			const Hom h = tilt_chain(p_id, t.canvas->tilt_floor);
			const ui::SwfColorXform cx = relative_color(p_id, t.canvas->color_floor);
			if (apply_quad(t.quad, t.quad_applied, xform_of(ui::swf_concat(stage_to_window, ui::swf_affine(n->world))), h, cx, 0, n->world_visible)) {
				mark(t.canvas);
			}
		}
	}

	ui::SwfBounds node_bounds(ui::SwfNodeId p_id, const Canvas &p_c) const {
		ui::SwfBounds b;
		const auto it = recs.find(p_id);
		const ui::SwfSceneNode *n = node(p_id);
		if (it == recs.end() || n == nullptr) {
			return b;
		}
		const NodeRec &rec = it->second;
		if (!rec.in_alpha && !n->world_visible) {
			return b;
		}
		const ui::SwfSceneNode *grid_root = rec.grid ? grid_owner(*n) : nullptr;
		const ui::SwfSceneNode *frame = grid_root != nullptr ? grid_root : n;
		const ui::SwfAffine m = ui::swf_concat(stage_to_window, ui::swf_affine(frame->world));
		const Hom h = tilt_chain(p_id, p_c.tilt_floor);
		for (const Piece &p : rec.pieces) {
			if (p.has_bounds) {
				b.add(project_rect(p.bounds, m, h));
			}
		}
		return b;
	}

	void relink_viewports() {
		for (Canvas *k : order) {
			if (k->kind != CanvasKind::Root) {
				link_viewports(*k);
			}
		}
		for (auto &[id, t] : targets) {
			if (t.canvas != nullptr && t.surface.viewport.is_valid()) {
				rs->viewport_set_parent_viewport(t.surface.viewport, t.canvas->content.viewport);
			}
		}
	}

	void update_layers() {
		bool relink = false;
		for (auto it = order.rbegin(); it != order.rend(); ++it) {
			Canvas &c = **it;
			if (c.kind == CanvasKind::Root || !c.bounds_dirty) {
				continue;
			}
			c.bounds_dirty = false;
			ui::SwfBounds b;
			for (const ui::SwfNodeId id : c.nodes) {
				b.add(node_bounds(id, c));
			}
			for (const Canvas *k : c.children) {
				if (!k->has_content) {
					continue;
				}
				const ui::SwfRect r{ float(k->origin.x), float(k->origin.x + k->extent.x), float(k->origin.y), float(k->origin.y + k->extent.y) };
				const Hom h = k->kind == CanvasKind::Filter ? tilt_chain(k->owner, c.tilt_floor) : Hom{};
				b.add(project_rect(r, ui::SwfAffine{}, h));
			}
			for (const ui::SwfNodeId tid : c.targets) {
				const auto tt = targets.find(tid);
				const ui::SwfSceneNode *n = node(tid);
				if (tt != targets.end() && n != nullptr && n->world_visible) {
					b.add(project_rect(tt->second.local, ui::swf_concat(stage_to_window, ui::swf_affine(n->world)), tilt_chain(tid, c.tilt_floor)));
				}
			}
			if (c.kind == CanvasKind::Mask) {
				ui::SwfBounds a;
				for (const ui::SwfNodeId id : c.alpha_nodes) {
					a.add(node_bounds(id, c));
				}
				b = b.intersect(a);
			}
			c.has_content = !b.empty();
			Vector2i origin(0, 0);
			Vector2i size(1, 1);
			if (c.has_content) {
				const double x0 = std::floor(b.x0 - c.margin_x) - 1.0;
				const double y0 = std::floor(b.y0 - c.margin_y) - 1.0;
				const double x1 = std::ceil(b.x1 + c.margin_x) + 1.0;
				const double y1 = std::ceil(b.y1 + c.margin_y) + 1.0;
				origin = Vector2i(int32_t(std::clamp(x0, -1e6, 1e6)), int32_t(std::clamp(y0, -1e6, 1e6)));
				size = Vector2i(int32_t(std::clamp(x1 - x0, 1.0, 1e7)), int32_t(std::clamp(y1 - y0, 1.0, 1e7)));
				if (size.x > kMaxLayerSide || size.y > kMaxLayerSide) {
					stats.layers_clamped++;
					size = Vector2i(std::min(size.x, kMaxLayerSide), std::min(size.y, kMaxLayerSide));
				}
			}
			c.extent = size;
			const RID final_texture = c.final_surface().texture;
			const bool kept = c.quad_size.x > 0 && !c.has_content;
			if (kept) {
				origin = c.quad_origin;
			}
			const bool fits = kept || (c.quad_size.x > 0 && layer_fits(c.size, size));
			const bool realloc = !fits || !final_texture.is_valid() || final_texture != c.quad_texture;
			if (realloc) {
				if (!fits) {
					c.size = Vector2i(layer_alloc(size.x), layer_alloc(size.y));
				}
				bool swapped = take(c.content, c.size, msaa);
				if (c.alpha.canvas.is_valid() && take(c.alpha, c.size, msaa)) {
					swapped = true;
					rs->material_set_param(c.quad_material, "mask_tex", c.alpha.texture);
				}
				RID input = c.content.texture;
				for (Stage &s : c.stages) {
					swapped = take(s.surface, c.size, RenderingServer::VIEWPORT_MSAA_DISABLED) || swapped;
					rs->canvas_item_clear(s.item);
					rs->canvas_item_add_texture_rect(s.item, Rect2(0.0f, 0.0f, real_t(c.size.x), real_t(c.size.y)), input);
					input = s.surface.texture;
				}
				if (swapped) {
					c.linked_parent = RID();
					relink = true;
				}
				c.quad_texture = c.final_surface().texture;
			}
			if (realloc || origin != c.quad_origin) {
				c.origin = origin;
				const Transform2D shift(1.0f, 0.0f, 0.0f, 1.0f, real_t(-origin.x), real_t(-origin.y));
				rs->viewport_set_canvas_transform(c.content.viewport, c.content.canvas, shift);
				if (c.alpha.viewport.is_valid()) {
					rs->viewport_set_canvas_transform(c.alpha.viewport, c.alpha.canvas, shift);
				}
				rs->canvas_item_clear(c.quad);
				rs->canvas_item_add_texture_rect(c.quad,
						Rect2(real_t(origin.x), real_t(origin.y), real_t(c.size.x), real_t(c.size.y)), c.quad_texture);
				c.quad_origin = origin;
				c.quad_size = c.size;
				c.render_dirty = true;
				mark(c.parent);
			}
			const bool resampled = c.kind == CanvasKind::Filter && !tilt_chain(c.owner, c.parent->tilt_floor).identity;
			const int32_t filter = resampled ? 1 : 0;
			if (filter != c.quad_filter) {
				rs->canvas_item_set_default_texture_filter(c.quad,
						resampled ? RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR : RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_NEAREST);
				c.quad_filter = filter;
				mark(c.parent);
			}
			const ui::SwfSceneNode *owner_node = node(c.owner);
			bool quad_changed = false;
			if (c.kind == CanvasKind::Mask) {
				quad_changed = apply_quad(c.quad, c.quad_applied, Transform2D(), Hom{}, ui::SwfColorXform{}, 0, c.has_content);
			} else if (owner_node != nullptr) {
				const Hom h = c.kind == CanvasKind::Filter ? tilt_chain(c.owner, c.parent->tilt_floor) : Hom{};
				const ui::SwfColorXform cx = relative_color(c.owner, c.parent->color_floor);
				quad_changed = apply_quad(c.quad, c.quad_applied, Transform2D(), h, cx, c.additive ? kFlagAdditive : 0, owner_node->world_visible && c.has_content);
			}
			if (quad_changed) {
				mark(c.parent);
			}
		}
		if (relink) {
			relink_viewports();
		}
	}

	bool hidden_from_render() const { return skips_hidden_render && !owner->is_visible_in_tree(); }

	void stop_targets() {
		for (auto &[id, t] : targets) {
			set_target_running(t, false);
		}
	}

	void render_dirty() {
		if (hidden_from_render()) {
			stop_targets();
			return;
		}
		for (auto &[id, t] : targets) {
			if (t.live && t.canvas != nullptr) {
				mark(t.canvas);
			}
		}
		for (Canvas *c : order) {
			if (!c->render_dirty) {
				continue;
			}
			c->render_dirty = false;
			stats.viewports_updated++;
			c->content.render(rs);
			c->alpha.render(rs);
			for (const Stage &s : c->stages) {
				s.surface.render(rs);
				stats.viewports_updated++;
			}
		}
		for (auto &[id, t] : targets) {
			if (t.live) {
				set_target_running(t, true);
			} else if (t.redraw) {
				t.surface.render(rs);
				t.redraw = false;
				if (t.canvas != nullptr) {
					mark(t.canvas);
				}
			}
		}
	}

	void present() {
		if (!present_item.is_valid()) {
			present_item = make_item();
			rs->canvas_item_set_parent(present_item, owner->get_canvas_item());
			rs->canvas_item_set_material(present_item, present_material);
			rs->canvas_item_set_default_texture_filter(present_item, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_NEAREST);
		}
		const auto it = canvases.find(canvas_key(CanvasKind::Root, 0));
		rs->canvas_item_clear(present_item);
		if (it == canvases.end()) {
			return;
		}
		rs->canvas_item_set_transform(present_item, owner->get_global_transform_with_canvas().affine_inverse());
		const Rect2 at = explicit_size ? Rect2(0.0f, 0.0f, real_t(target_size.x), real_t(target_size.y)) : present_rect;
		rs->canvas_item_add_texture_rect(present_item, at, it->second.content.texture);
	}

	void sync() {
		if (!scene || !owner->is_inside_tree()) {
			return;
		}
		const auto t0 = std::chrono::steady_clock::now();
		if (!explicit_size && owner->is_inside_tree() && owner->get_viewport() != nullptr) {
			const Rect2 vis = ui_window_rect(owner);
			const Vector2i s = ui_window_pixels(owner);
			if (vis != present_rect) {
				present_rect = vis;
				structure_dirty = true;
			}
			if (s != target_size) {
				target_size = s;
				structure_dirty = true;
			}
		}
		if (owner->is_inside_tree() && owner->get_viewport() != nullptr) {
			const RID vp = owner->get_viewport()->get_viewport_rid();
			if (vp != owner_viewport) {
				owner_viewport = vp;
				structure_dirty = true;
			}
		}
		const std::vector<ui::SwfSceneChange> changes = scene->take_changes();
		constexpr uint32_t structural = ui::swf_change::kCreated | ui::swf_change::kRemoved | ui::swf_change::kChildren | ui::swf_change::kMask |
				ui::swf_change::kEffects;
		std::vector<ui::SwfNodeId> touched;
		for (const ui::SwfSceneChange &ch : changes) {
			if ((ch.what & structural) != 0) {
				structure_dirty = true;
			}
			if ((ch.what & ui::swf_change::kCharacter) != 0) {
				const ui::SwfSceneNode *cn = node(ch.node);
				if (cn == nullptr || !cn->children.empty() || (cn->shape == nullptr && cn->sub_image == nullptr && cn->edit_text == nullptr)) {
					structure_dirty = true;
				}
			}
			if ((ch.what & (ui::swf_change::kText | ui::swf_change::kCharacter)) != 0 && !structure_dirty) {
				const auto it = recs.find(ch.node);
				if (it != recs.end() && it->second.built) {
					const size_t before = it->second.pieces.size();
					build(ch.node, it->second);
					if (it->second.pieces.size() != before) {
						structure_dirty = true;
					}
				}
			}
			if ((ch.what & (ui::swf_change::kMatrix | ui::swf_change::kColor | ui::swf_change::kVisible | ui::swf_change::kText | ui::swf_change::kCharacter)) != 0) {
				touched.push_back(ch.node);
			}
		}
		const uint32_t changes_count = uint32_t(changes.size());
		stats.nodes_refreshed = 0;
		stats.viewports_updated = 0;
		for (int pass = 0; pass < 2; ++pass) {
			if (structure_dirty) {
				update_fit();
				plan();
				touched.clear();
				for (auto &[id, rec] : recs) {
					(void)rec;
					touched.push_back(id);
				}
				for (auto &[id, t] : targets) {
					(void)t;
					touched.push_back(id);
				}
			}
			if (items_created > 0) {
				rs->force_sync();
				items_created = 0;
			}
			for (const ui::SwfNodeId id : touched) {
				refresh(id);
			}
			if (!structure_dirty) {
				break;
			}
		}
		update_layers();
		render_dirty();
		present();
		stats.changes = changes_count;
		if (settle_syncs < Bf3SwfView::kDormantSettleSyncs && ++settle_syncs == Bf3SwfView::kDormantSettleSyncs) {
			trim_dormant();
		}
		stats.sync_seconds = bf3::core::seconds_since(t0);
		g_sync_seconds += stats.sync_seconds;
	}
};

Bf3SwfView::Bf3SwfView() :
		impl_(std::make_unique<Impl>()) {
	impl_->owner = this;
	g_views.push_back(this);
}

Bf3SwfView::~Bf3SwfView() {
	g_views.erase(std::remove(g_views.begin(), g_views.end(), this), g_views.end());
}

void Bf3SwfView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open_movie", "movie"), &Bf3SwfView::open_movie);
	ClassDB::bind_method(D_METHOD("get_error"), &Bf3SwfView::get_error);
	ClassDB::bind_method(D_METHOD("set_playing", "playing"), &Bf3SwfView::set_playing);
	ClassDB::bind_method(D_METHOD("is_playing"), &Bf3SwfView::is_playing);
	ClassDB::bind_method(D_METHOD("set_tilt_degrees", "path", "x_rotation", "y_rotation", "perspective_fov"), &Bf3SwfView::set_tilt_degrees);
	ClassDB::bind_method(D_METHOD("set_renderer", "path", "renderer"), &Bf3SwfView::set_renderer);
	ClassDB::bind_method(D_METHOD("get_hud_image"), &Bf3SwfView::get_hud_image);
	ClassDB::bind_method(D_METHOD("sync"), &Bf3SwfView::sync);
}

void Bf3SwfView::_ready() {
	set_process(true);
}

void Bf3SwfView::_notification(int p_what) {
	if (p_what == NOTIFICATION_VISIBILITY_CHANGED && impl_->hidden_from_render()) {
		impl_->stop_targets();
	}
}

void Bf3SwfView::_process(double p_delta) {
	if (!impl_->scene) {
		return;
	}
	if (impl_->playing) {
		const Status st = impl_->scene->advance_seconds(p_delta);
		if (!st) {
			impl_->warn_once(st.error().message);
		}
	}
	sync();
}

Status Bf3SwfView::open(std::string_view p_movie) {
	Bf3UiAssets *assets = Bf3UiAssets::current();
	if (assets == nullptr || !assets->is_loaded() || assets->movies() == nullptr) {
		close();
		impl_->error = "Bf3UiAssets is not loaded";
		return make_error(impl_->error);
	}
	bf3::core::Result<ui::SwfScene> scene = ui::SwfScene::open(*assets->movies(), p_movie);
	if (!scene) {
		close();
		impl_->error = scene.error().message;
		return scene.error();
	}
	if (impl_->scene && impl_->assets == assets && impl_->movie == scene->movie_name()) {
		impl_->retire_scene();
		impl_->stats.reopens++;
	} else {
		close();
	}
	impl_->assets = assets;
	impl_->scene.emplace(std::move(*scene));
	impl_->textures = Bf3UiTextures::shared();
	impl_->scene->set_image_source(impl_->textures);
	impl_->movie = impl_->scene->movie_name();
	impl_->error.clear();
	impl_->structure_dirty = true;
	impl_->update_fit();
	impl_->open_serial = ++g_open_serial;
	impl_->settle_syncs = 0;
	return Status::success();
}

Status Bf3SwfView::open(std::shared_ptr<const ui::SwfLibrary> p_library, std::string_view p_movie) {
	close();
	if (p_library == nullptr) {
		impl_->error = "no SwfLibrary";
		return make_error(impl_->error);
	}
	bf3::core::Result<ui::SwfScene> scene = ui::SwfScene::open(*p_library, p_movie);
	if (!scene) {
		impl_->error = scene.error().message;
		return scene.error();
	}
	impl_->assets = Bf3UiAssets::current();
	impl_->library = std::move(p_library);
	impl_->scene.emplace(std::move(*scene));
	impl_->textures = Bf3UiTextures::shared();
	impl_->scene->set_image_source(impl_->textures);
	impl_->movie = impl_->scene->movie_name();
	impl_->error.clear();
	impl_->structure_dirty = true;
	impl_->update_fit();
	impl_->open_serial = ++g_open_serial;
	impl_->settle_syncs = 0;
	return Status::success();
}

void Bf3SwfView::close() {
	impl_->release();
}

uint64_t Bf3SwfView::surface_bytes() const {
	uint64_t out = 0;
	for (const auto &[k, c] : impl_->canvases) {
		out += impl_->canvas_bytes(c);
	}
	for (const auto &[k, c] : impl_->dormant_canvases) {
		out += impl_->canvas_bytes(c);
	}
	for (const auto &[id, t] : impl_->targets) {
		out += t.surface.bytes(impl_->msaa);
	}
	for (const SpareSurface &sp : impl_->spares) {
		out += target_bytes(sp.size, sp.msaa);
	}
	return out;
}

uint32_t Bf3SwfView::instance_items() const {
	size_t out = impl_->canvases.size() + impl_->dormant_canvases.size() + impl_->targets.size();
	for (const auto &[id, rec] : impl_->recs) {
		out += rec.pieces.size();
	}
	for (const auto &[k, rec] : impl_->dormant_recs) {
		out += rec.pieces.size();
	}
	return uint32_t(out);
}

uint64_t Bf3SwfView::open_serial() const {
	return impl_->open_serial;
}

double Bf3SwfView::sync_seconds_total() {
	return g_sync_seconds;
}

SwfViewCounters Bf3SwfView::counters() {
	SwfViewCounters out = g_counters;
	out.retained_bytes = 0;
	for (const Bf3SwfView *v : g_views) {
		out.retained_bytes += v->surface_bytes();
	}
	return out;
}

void Bf3SwfView::release_root_target() {
	const auto it = impl_->canvases.find(canvas_key(CanvasKind::Root, 0));
	if (it != impl_->canvases.end() && it->second.content.resize(impl_->rs, Vector2i(1, 1))) {
		impl_->structure_dirty = true;
	}
}

bool Bf3SwfView::is_open() const {
	return impl_->scene.has_value();
}

const std::string &Bf3SwfView::error() const {
	return impl_->error;
}

ui::SwfScene *Bf3SwfView::scene() {
	return impl_->scene ? &*impl_->scene : nullptr;
}

const ui::SwfScene *Bf3SwfView::scene() const {
	return impl_->scene ? &*impl_->scene : nullptr;
}

void Bf3SwfView::set_playing(bool p_playing) {
	impl_->playing = p_playing;
}

bool Bf3SwfView::is_playing() const {
	return impl_->playing;
}

Status Bf3SwfView::set_tilt(std::string_view p_path, const SwfTilt &p_tilt) {
	if (!impl_->scene || impl_->scene->find(p_path) == ui::kSwfNoNode) {
		return make_error("swf view: no node at " + std::string(p_path));
	}
	impl_->tilt_paths[std::string(p_path)] = p_tilt;
	impl_->structure_dirty = true;
	return Status::success();
}

Status Bf3SwfView::clear_tilt(std::string_view p_path) {
	impl_->tilt_paths.erase(std::string(p_path));
	impl_->structure_dirty = true;
	return Status::success();
}

Status Bf3SwfView::set_renderer_string(std::string_view p_path, std::string_view p_renderer) {
	if (!impl_->scene || impl_->scene->find(p_path) == ui::kSwfNoNode) {
		return make_error("swf view: no node at " + std::string(p_path));
	}
	impl_->renderer_paths[std::string(p_path)] = std::string(p_renderer);
	impl_->structure_dirty = true;
	return Status::success();
}

std::vector<SwfEngineTarget> Bf3SwfView::engine_targets() const {
	std::vector<SwfEngineTarget> out;
	for (const auto &[id, t] : impl_->targets) {
		SwfEngineTarget e;
		e.renderer = t.renderer;
		e.path = t.path;
		e.node = t.node;
		e.local_bounds = t.local;
		e.size = t.surface.size;
		e.canvas_item = t.root_item;
		e.texture = t.surface.texture;
		e.live = t.live;
		out.push_back(std::move(e));
	}
	return out;
}

std::vector<SwfEngineTarget> Bf3SwfView::engine_targets(std::string_view p_renderer) const {
	std::vector<SwfEngineTarget> out;
	for (SwfEngineTarget &e : engine_targets()) {
		if (e.renderer == p_renderer) {
			out.push_back(std::move(e));
		}
	}
	return out;
}

Status Bf3SwfView::redraw_engine_target(std::string_view p_path) {
	for (auto &[id, t] : impl_->targets) {
		if (t.path == p_path) {
			t.redraw = true;
			return Status::success();
		}
	}
	return make_error("swf view: no engine target at " + std::string(p_path));
}

Status Bf3SwfView::set_engine_target_live(std::string_view p_path, bool p_live) {
	for (auto &[id, t] : impl_->targets) {
		if (t.path == p_path) {
			t.live = p_live;
			if (!p_live) {
				impl_->set_target_running(t, false);
				t.redraw = true;
			}
			return Status::success();
		}
	}
	return make_error("swf view: no engine target at " + std::string(p_path));
}

void Bf3SwfView::set_scale_mode(SwfScaleMode p_mode) {
	impl_->scale_mode = p_mode;
	impl_->structure_dirty = true;
}

SwfScaleMode Bf3SwfView::scale_mode() const {
	return impl_->scale_mode;
}

void Bf3SwfView::set_target_size(Vector2i p_size) {
	impl_->explicit_size = p_size.x > 0 && p_size.y > 0;
	if (impl_->explicit_size) {
		impl_->target_size = p_size;
	}
	impl_->structure_dirty = true;
}

Vector2i Bf3SwfView::target_size() const {
	return impl_->target_size;
}

SwfStageFit Bf3SwfView::stage_fit() const {
	return impl_->fit;
}

Rect2 Bf3SwfView::present_rect() const {
	return impl_->explicit_size ? Rect2(0.0f, 0.0f, real_t(impl_->target_size.x), real_t(impl_->target_size.y))
								: impl_->present_rect;
}

std::optional<ui::SwfPoint> Bf3SwfView::canvas_to_stage(Vector2 p_canvas) const {
	const SwfStageFit &f = impl_->fit;
	const Rect2 at = present_rect();
	if (f.scale_x == 0.0 || f.scale_y == 0.0 || !(at.size.x > 0.0f) || !(at.size.y > 0.0f)) {
		return std::nullopt;
	}
	const Vector2 local = p_canvas - at.position;
	const double px = double(local.x) * double(impl_->target_size.x) / double(at.size.x);
	const double py = double(local.y) * double(impl_->target_size.y) / double(at.size.y);
	return ui::SwfPoint{ float((px - f.offset_x) / f.scale_x), float((py - f.offset_y) / f.scale_y) };
}

Vector2 Bf3SwfView::stage_to_canvas(ui::SwfPoint p_stage) const {
	const SwfStageFit &f = impl_->fit;
	const Rect2 at = present_rect();
	const double px = double(p_stage.x) * f.scale_x + f.offset_x;
	const double py = double(p_stage.y) * f.scale_y + f.offset_y;
	const Vector2 local(float(px * double(at.size.x) / double(std::max(1, impl_->target_size.x))),
			float(py * double(at.size.y) / double(std::max(1, impl_->target_size.y))));
	return local + at.position;
}

ui::SwfRect Bf3SwfView::visible_rect() const {
	const SwfStageFit &f = impl_->fit;
	return ui::SwfRect{ float(-f.offset_x / f.scale_x), float((double(impl_->target_size.x) - f.offset_x) / f.scale_x), float(-f.offset_y / f.scale_y),
		float((double(impl_->target_size.y) - f.offset_y) / f.scale_y) };
}

uint32_t Bf3SwfView::filter_stages_without_viewport() const {
	uint32_t count = 0;
	for (const auto &[k, c] : impl_->canvases) {
		count += uint32_t(std::count_if(
				c.stages.begin(), c.stages.end(), [](const Stage &p_s) { return !p_s.surface.viewport.is_valid(); }));
	}
	return count;
}

void Bf3SwfView::set_skips_hidden_render(bool p_skips) {
	impl_->skips_hidden_render = p_skips;
}

void Bf3SwfView::set_msaa(RenderingServer::ViewportMSAA p_msaa) {
	impl_->msaa = p_msaa;
	impl_->free_spares();
	for (auto &[k, c] : impl_->canvases) {
		if (c.content.viewport.is_valid()) {
			impl_->rs->viewport_set_msaa_2d(c.content.viewport, p_msaa);
		}
		if (c.alpha.viewport.is_valid()) {
			impl_->rs->viewport_set_msaa_2d(c.alpha.viewport, p_msaa);
		}
		c.render_dirty = true;
	}
}

Status Bf3SwfView::set_isolated(std::string_view p_path) {
	if (!impl_->scene || impl_->scene->find(p_path) == ui::kSwfNoNode) {
		return make_error("swf view: no node at " + std::string(p_path));
	}
	impl_->isolated = std::string(p_path);
	impl_->structure_dirty = true;
	return Status::success();
}

void Bf3SwfView::clear_isolation() {
	impl_->isolated.clear();
	impl_->structure_dirty = true;
}

void Bf3SwfView::set_filters_enabled(bool p_enabled) {
	impl_->filters_enabled = p_enabled;
	impl_->structure_dirty = true;
}

void Bf3SwfView::set_additive_coverage(bool p_coverage) {
	impl_->rs->material_set_param(impl_->item_material, "coverage", p_coverage ? 1 : 0);
	for (auto &[k, c] : impl_->canvases) {
		c.render_dirty = true;
	}
	for (auto &[id, t] : impl_->targets) {
		t.redraw = true;
	}
}

void Bf3SwfView::sync() {
	impl_->sync();
}

RID Bf3SwfView::hud_texture() const {
	const auto it = impl_->canvases.find(canvas_key(CanvasKind::Root, 0));
	return it == impl_->canvases.end() ? RID() : it->second.content.texture;
}

Ref<Image> Bf3SwfView::capture_hud() const {
	const RID t = hud_texture();
	return t.is_valid() ? impl_->rs->texture_2d_get(t) : Ref<Image>();
}

const SwfViewStats &Bf3SwfView::stats() const {
	return impl_->stats;
}

bool Bf3SwfView::open_movie(const String &p_movie) {
	return bool(open(to_std(p_movie)));
}

String Bf3SwfView::get_error() const {
	return to_godot(impl_->error);
}

bool Bf3SwfView::set_tilt_degrees(const String &p_path, float p_x_rotation, float p_y_rotation, float p_perspective_fov) {
	return bool(set_tilt(to_std(p_path), SwfTilt{ p_x_rotation, p_y_rotation, p_perspective_fov }));
}

bool Bf3SwfView::set_renderer(const String &p_path, const String &p_renderer) {
	return bool(set_renderer_string(to_std(p_path), to_std(p_renderer)));
}

Ref<Image> Bf3SwfView::get_hud_image() const {
	return capture_hud();
}

Ref<Image> swf_view_transmittance(std::span<const Ref<Image>> p_colour) {
	int32_t w = 0;
	int32_t h = 0;
	std::vector<double> t;
	for (const Ref<Image> &src : p_colour) {
		if (src.is_null() || src->is_empty()) {
			continue;
		}
		Ref<Image> img = src;
		if (img->get_format() != Image::FORMAT_RGBA8) {
			img = src->duplicate();
			img->convert(Image::FORMAT_RGBA8);
		}
		if (t.empty()) {
			w = img->get_width();
			h = img->get_height();
			t.assign(size_t(w) * size_t(h), 1.0);
		} else if (img->get_width() != w || img->get_height() != h) {
			return Ref<Image>();
		}
		const PackedByteArray d = img->get_data();
		const uint8_t *px = d.ptr();
		for (size_t i = 0; i < t.size(); ++i) {
			t[i] *= 1.0 - double(px[i * 4 + 3]) / 255.0;
		}
	}
	if (t.empty()) {
		return Ref<Image>();
	}
	PackedByteArray bytes;
	bytes.resize(int64_t(t.size()));
	uint8_t *out = bytes.ptrw();
	for (size_t i = 0; i < t.size(); ++i) {
		out[i] = uint8_t(std::clamp(std::lround(255.0 * t[i]), 0L, 255L));
	}
	return Image::create_from_data(w, h, false, Image::FORMAT_L8, bytes);
}

}
