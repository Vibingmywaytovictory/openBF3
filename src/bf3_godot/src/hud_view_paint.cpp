#include "hud_view_paint.h"

#include "godot_util.h"
#include "swf_view.h"

#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;
using namespace godot;

namespace {

constexpr std::string_view kBgDrawShader = R"(shader_type canvas_item;
render_mode blend_premul_alpha;
)";

struct BgAxis {
	std::array<float, 10> at{};
	std::array<float, 10> light{};
	std::array<float, 10> coverage{};
};

BgAxis bg_axis(float p_min, float p_max, float p_fade_fraction, const ui::HudBgDrawLook &p_look) {
	const float fade = p_fade_fraction * (p_max - p_min);
	const float half = (p_max - p_min) / 2.0f;
	std::array<float, 5> inward{ 0.0f, p_look.coverage_start_of_fade, p_look.coverage_full_of_fade,
		p_look.light_start_of_fade, p_look.light_full_of_fade };
	for (float &d : inward) {
		d = std::min(fade * d, half);
	}
	std::sort(inward.begin(), inward.end());
	BgAxis a;
	for (size_t i = 0; i < inward.size(); ++i) {
		a.at[i] = p_min + inward[i];
		a.at[a.at.size() - 1 - i] = p_max - inward[i];
	}
	for (size_t i = 0; i < a.at.size(); ++i) {
		const float u = fade > 0.0f ? std::min(a.at[i] - p_min, p_max - a.at[i]) / fade : 1.0f;
		a.light[i] = fade > 0.0f ? ui::hud_bg_light_ramp(p_look, u) : 1.0f;
		a.coverage[i] = fade > 0.0f ? ui::hud_bg_coverage_ramp(p_look, u) : 1.0f;
	}
	return a;
}

}

bool HudBgPainter::paint(Bf3SwfView &r_view, const SwfEngineTarget &p_target, std::string_view p_owner) {
	if (!p_target.canvas_item.is_valid() || p_target.renderer != ui::kBgDrawRenderer) {
		return false;
	}
	const std::string key = std::string(p_owner) + "|" + p_target.path;
	const auto it = painted_.find(key);
	if (it != painted_.end() && it->second.first == p_target.local_bounds && it->second.second == p_target.size) {
		return false;
	}
	painted_[key] = { p_target.local_bounds, p_target.size };
	RenderingServer *rs = RenderingServer::get_singleton();
	if (material_.is_null()) {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(String::utf8(kBgDrawShader.data(), int64_t(kBgDrawShader.size())));
		material_.instantiate();
		material_->set_shader(shader);
	}
	rs->canvas_item_clear(p_target.canvas_item);
	rs->canvas_item_set_material(p_target.canvas_item, material_->get_rid());
	const ui::HudBgDrawLook &k = ui::kHudBgDrawLook;
	const ui::SwfRect &b = p_target.local_bounds;
	const BgAxis ax = bg_axis(b.x_min, b.x_max, float(ui::hud_bg_light_ramp_fraction_x()), k);
	const BgAxis ay = bg_axis(b.y_min, b.y_max, float(ui::hud_bg_light_ramp_fraction_y()), k);
	const auto colour_at = [&](size_t p_ix, size_t p_iy) {
		const float l = ax.light[p_ix] * ay.light[p_iy];
		return Color(k.premul_rgb255[0] / 255.0f * l, k.premul_rgb255[1] / 255.0f * l, k.premul_rgb255[2] / 255.0f * l,
				k.coverage * ax.coverage[p_ix] * ay.coverage[p_iy]);
	};
	for (size_t iy = 0; iy + 1 < ay.at.size(); ++iy) {
		for (size_t ix = 0; ix + 1 < ax.at.size(); ++ix) {
			if (!(ax.at[ix + 1] > ax.at[ix]) || !(ay.at[iy + 1] > ay.at[iy])) {
				continue;
			}
			PackedVector2Array pts;
			PackedColorArray cols;
			const size_t corner[4][2] = { { ix, iy }, { ix + 1, iy }, { ix + 1, iy + 1 }, { ix, iy + 1 } };
			for (const auto &c : corner) {
				pts.push_back(Vector2(ax.at[c[0]], ay.at[c[1]]));
				cols.push_back(colour_at(c[0], c[1]));
			}
			rs->canvas_item_add_polygon(p_target.canvas_item, pts, cols);
		}
	}
	if (const bf3::core::Status st = r_view.redraw_engine_target(p_target.path); !st) {
		UtilityFunctions::push_warning(to_godot("[hud] engine target " + p_target.path + ": " + st.error().message));
	}
	return true;
}

void HudBgPainter::forget(std::string_view p_owner) {
	const std::string prefix = std::string(p_owner) + "|";
	for (auto it = painted_.lower_bound(prefix); it != painted_.end() && it->first.starts_with(prefix);) {
		it = painted_.erase(it);
	}
}

void HudBgPainter::paint_all(Bf3SwfView &r_view, std::string_view p_owner) {
	for (const SwfEngineTarget &t : r_view.engine_targets(ui::kBgDrawRenderer)) {
		paint(r_view, t, p_owner);
	}
}

void apply_hud_view_effects(Bf3SwfView &r_view, const ui::HudScreen &p_screen) {
	for (const ui::HudTilt &t : p_screen.tilts()) {
		const bf3::core::Status st = r_view.set_tilt(t.path, SwfTilt{ t.x_rotation, t.y_rotation, t.perspective_fov });
		if (!st) {
			UtilityFunctions::push_warning(to_godot("[hud] tilt " + t.path + ": " + st.error().message));
		}
	}
	for (const ui::HudRendererString &r : p_screen.renderer_strings()) {
		const bf3::core::Status st = r_view.set_renderer_string(r.path, r.renderer);
		if (!st) {
			UtilityFunctions::push_warning(to_godot("[hud] renderer " + r.path + ": " + st.error().message));
		}
	}
}

}
