#include "nav_overlay.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bf3::godot_side {

using namespace godot;

namespace {

// Unshaded vertex colour, both faces, depth tested; a little transparent so the floor shows through.
const char *kNavShader = R"(shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque, shadows_disabled;
void fragment() {
	ALBEDO = COLOR.rgb;
	ALPHA = COLOR.a;
}
)";

std::array<float, 4> hue_color(float p_hue, float p_s, float p_v, float p_a) {
	const float h = (p_hue - std::floor(p_hue)) * 6.0f;
	const float c = p_v * p_s;
	const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
	const float m = p_v - c;
	float r = 0, g = 0, b = 0;
	if (h < 1) {
		r = c, g = x;
	} else if (h < 2) {
		r = x, g = c;
	} else if (h < 3) {
		g = c, b = x;
	} else if (h < 4) {
		g = x, b = c;
	} else if (h < 5) {
		r = x, b = c;
	} else {
		r = c, b = x;
	}
	return { r + m, g + m, b + m, p_a };
}

} // namespace

NavOverlayArrays prepare_nav_overlay(const sim::NavDebugMesh &p_mesh, uint32_t p_main) {
	NavOverlayArrays out;
	const size_t triangles = std::min(p_mesh.triangles.size() / 3, std::min(p_mesh.triangle_component.size(), p_mesh.triangle_area.size()));
	out.triangle_positions.reserve(triangles * 3);
	out.triangle_colors.reserve(triangles * 3);
	for (size_t t = 0; t < triangles; ++t) {
		const uint32_t comp = p_mesh.triangle_component[t];
		const bool main = comp == p_main;
		std::array<float, 4> c;
		if (main) {
			c = p_mesh.triangle_area[t] == sim::kNavAreaCrouch ? std::array<float, 4>{ 0.15f, 0.35f, 1.0f, 0.6f } : std::array<float, 4>{ 0.1f, 0.85f, 0.2f, 0.55f };
			++out.triangles_main;
		} else {
			// Red to magenta, a hue per component id (golden-ratio steps over a 0.12-wide band around red).
			const float f = float(std::fmod(double(comp) * 0.61803398875, 1.0));
			c = hue_color(0.94f + 0.12f * f, 0.9f, 0.95f, 0.6f);
			++out.triangles_other;
		}
		for (int k = 0; k < 3; ++k) {
			const sim::Vec3 &v = p_mesh.triangles[t * 3 + size_t(k)];
			out.triangle_positions.push_back({ v[0], v[1], v[2] });
			out.triangle_colors.push_back(c);
		}
	}
	out.links = p_mesh.links.size() / 2;
	out.line_positions.reserve(out.links * 2);
	for (size_t i = 0; i < out.links * 2; ++i) {
		const sim::Vec3 &v = p_mesh.links[i];
		out.line_positions.push_back({ v[0], v[1], v[2] });
	}
	return out;
}

bool NavOverlay::upload(const NavOverlayArrays &p_arrays, const RID &p_scenario, bool p_visible) {
	if (p_arrays.triangle_positions.empty()) {
		return false;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	shader_ = rs->shader_create();
	rs->shader_set_code(shader_, kNavShader);
	material_ = rs->material_create();
	rs->material_set_shader(material_, shader_);
	mesh_ = rs->mesh_create();
	{
		const int64_t n = int64_t(p_arrays.triangle_positions.size());
		PackedVector3Array positions;
		positions.resize(n);
		std::memcpy(positions.ptrw(), p_arrays.triangle_positions.data(), size_t(n) * sizeof(Vector3));
		PackedColorArray colors;
		colors.resize(n);
		std::memcpy(colors.ptrw(), p_arrays.triangle_colors.data(), size_t(n) * sizeof(Color));
		Array arrays;
		arrays.resize(RenderingServer::ARRAY_MAX);
		arrays[RenderingServer::ARRAY_VERTEX] = positions;
		arrays[RenderingServer::ARRAY_COLOR] = colors;
		rs->mesh_add_surface_from_arrays(mesh_, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
		rs->mesh_surface_set_material(mesh_, 0, material_);
	}
	if (!p_arrays.line_positions.empty()) {
		const int64_t n = int64_t(p_arrays.line_positions.size());
		PackedVector3Array positions;
		positions.resize(n);
		std::memcpy(positions.ptrw(), p_arrays.line_positions.data(), size_t(n) * sizeof(Vector3));
		PackedColorArray colors;
		colors.resize(n);
		for (int64_t i = 0; i < n; ++i) {
			colors.set(i, Color(1.0f, 0.9f, 0.1f, 1.0f));
		}
		Array arrays;
		arrays.resize(RenderingServer::ARRAY_MAX);
		arrays[RenderingServer::ARRAY_VERTEX] = positions;
		arrays[RenderingServer::ARRAY_COLOR] = colors;
		rs->mesh_add_surface_from_arrays(mesh_, RenderingServer::PRIMITIVE_LINES, arrays);
		rs->mesh_surface_set_material(mesh_, 1, material_);
	}
	instance_ = rs->instance_create2(mesh_, p_scenario);
	rs->instance_geometry_set_cast_shadows_setting(instance_, RenderingServer::SHADOW_CASTING_SETTING_OFF);
	set_visible(p_visible);
	return true;
}

void NavOverlay::set_visible(bool p_visible) {
	visible_ = p_visible;
	if (instance_.is_valid()) {
		RenderingServer::get_singleton()->instance_set_visible(instance_, p_visible);
	}
}

void NavOverlay::free_all() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	for (RID *r : { &instance_, &mesh_, &material_, &shader_ }) {
		if (r->is_valid()) {
			rs->free_rid(*r);
			*r = RID();
		}
	}
}

} // namespace bf3::godot_side
