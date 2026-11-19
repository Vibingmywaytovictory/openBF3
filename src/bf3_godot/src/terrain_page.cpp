#include "terrain_page.h"

#include "bf3/core/util/format.h"
#include "bf3/render/terrain_page.h"
#include "godot_util.h"
#include "terrain_detail.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

using namespace godot;
namespace render = bf3::render;
using bf3::core::fmt;

namespace {

constexpr float kUnshownOrigin = 1.0e5f;
constexpr float kCameraClearance = 1.0f;

}

TerrainPageDraw::~TerrainPageDraw() {
	free_all();
}

TerrainPageDraw::Page TerrainPageDraw::make_page(uint32_t p_layer) {
	RenderingServer *rs = RenderingServer::get_singleton();
	Page page;
	page.camera = rs->camera_create();
	rs->camera_set_cull_mask(page.camera, p_layer);
	rs->camera_set_environment(page.camera, environment_);
	page.viewport = rs->viewport_create();
	rs->viewport_set_size(page.viewport, int32_t(render::kTerrainPageSide), int32_t(render::kTerrainPageSide));
	rs->viewport_set_use_hdr_2d(page.viewport, true);
	rs->viewport_set_disable_2d(page.viewport, true);
	rs->viewport_set_transparent_background(page.viewport, false);
	rs->viewport_set_positional_shadow_atlas_size(page.viewport, 0);
	rs->viewport_set_scenario(page.viewport, scenario_);
	rs->viewport_attach_camera(page.viewport, page.camera);
	rs->viewport_set_update_mode(page.viewport, RenderingServer::VIEWPORT_UPDATE_DISABLED);
	rs->viewport_set_active(page.viewport, true);
	page.texture = rs->viewport_get_texture(page.viewport);
	return page;
}

void TerrainPageDraw::aim(const Page &p_page, float p_x, float p_z, float p_side) const {
	RenderingServer *rs = RenderingServer::get_singleton();
	const float depth = high_ - low_ + 2.0f * kCameraClearance;
	rs->camera_set_orthogonal(p_page.camera, p_side, 0.5f * kCameraClearance, depth);
	const Basis down(Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, -1.0f), Vector3(0.0f, 1.0f, 0.0f));
	rs->camera_set_transform(p_page.camera, Transform3D(down, Vector3(p_x, high_ + kCameraClearance, p_z)));
	rs->viewport_set_update_mode(p_page.viewport, RenderingServer::VIEWPORT_UPDATE_ONCE);
}

void TerrainPageDraw::set_param(const char *p_name, const Variant &p_value) const {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (const RID &material : materials_) {
		rs->material_set_param(material, StringName(p_name), p_value);
	}
}

void TerrainPageDraw::begin(std::string p_name, const RID &p_terrain_mesh, const Transform3D &p_terrain,
		const AABB &p_world_bounds, float p_near_texel, std::vector<RID> p_materials) {
	free_all();
	if (!p_terrain_mesh.is_valid() || p_materials.empty() || !(p_near_texel > 0.0f)) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	name_ = std::move(p_name);
	materials_ = std::move(p_materials);
	low_ = p_world_bounds.position.y;
	high_ = p_world_bounds.position.y + p_world_bounds.size.y;
	near_side_ = float(render::kTerrainPageSide) * p_near_texel;
	scenario_ = rs->scenario_create();
	environment_ = rs->environment_create();
	rs->environment_set_background(environment_, RenderingServer::ENV_BG_COLOR);
	rs->environment_set_bg_color(environment_, Color(0.0f, 0.0f, 0.0f));
	rs->environment_set_ambient_light(environment_, Color(0.0f, 0.0f, 0.0f),
			RenderingServer::ENV_AMBIENT_SOURCE_DISABLED, 0.0f, 0.0f, RenderingServer::ENV_REFLECTION_SOURCE_DISABLED);
	rs->environment_set_tonemap(environment_, RenderingServer::ENV_TONE_MAPPER_LINEAR, 1.0f, 1.0f);
	instance_ = rs->instance_create2(p_terrain_mesh, scenario_);
	rs->instance_set_transform(instance_, p_terrain);
	rs->instance_set_layer_mask(instance_, render::kTerrainPageColourLayer | render::kTerrainPageNormalLayer);
	near_ = { make_page(render::kTerrainPageColourLayer), make_page(render::kTerrainPageNormalLayer) };
	const float far_side = std::max(p_world_bounds.size.x, p_world_bounds.size.z);
	const float cx = p_world_bounds.position.x + 0.5f * p_world_bounds.size.x;
	const float cz = p_world_bounds.position.z + 0.5f * p_world_bounds.size.z;
	far_centre_ = { cx, cz };
	for (const Page &page : near_) {
		aim(page, cx, cz, far_side);
	}
	far_pending_ = true;
	requested_frame_ = Engine::get_singleton()->get_frames_drawn();
	set_param(render::kTerrainPageUvUniform.data(),
			vec4_of(render::terrain_page_uv(kUnshownOrigin, kUnshownOrigin, near_side_)));
	set_param(
			"terrain_page_far", vec4_of(render::terrain_page_uv(cx - 0.5f * far_side, cz - 0.5f * far_side, far_side)));
	stats_.near_texel = p_near_texel;
	stats_.far_side = far_side;
	stats_.far_texel = far_side / float(render::kTerrainPageSide);
	stats_.materials = materials_.size();
}

void TerrainPageDraw::update(const Vector3 &p_camera, uint64_t p_ground) {
	if (!scenario_.is_valid()) {
		return;
	}
	if ((far_pending_ || pending_) && Engine::get_singleton()->get_frames_drawn() <= requested_frame_) {
		return;
	}
	if (far_pending_) {
		far_pending_ = false;
		RenderingServer *rs = RenderingServer::get_singleton();
		for (size_t i = 0; i < near_.size(); ++i) {
			const Ref<Image> image = rs->texture_2d_get(near_[i].texture);
			if (image.is_valid() && !image->is_empty()) {
				far_textures_[i] = rs->texture_2d_create(image);
			}
		}
		set_param("terrain_page_colour", near_[0].texture);
		set_param("terrain_page_normal", near_[1].texture);
		set_param("terrain_page_far_colour", far_textures_[0]);
		set_param("terrain_page_far_normal", far_textures_[1]);
		ground_ = p_ground;
		for (const Page &page : near_) {
			aim(page, p_camera.x, p_camera.z, near_side_);
		}
		pending_ = std::array<float, 2>{ p_camera.x, p_camera.z };
		requested_frame_ = Engine::get_singleton()->get_frames_drawn();
		return;
	}
	if (pending_) {
		const std::array<float, 2> centre = *pending_;
		pending_.reset();
		set_param(render::kTerrainPageUvUniform.data(),
				vec4_of(render::terrain_page_uv(
						centre[0] - 0.5f * near_side_, centre[1] - 0.5f * near_side_, near_side_)));
		shown_ = centre;
		++stats_.near_windows;
		report(p_camera);
		return;
	}
	const bool ground = !ground_ || *ground_ != p_ground;
	if (shown_ && !ground) {
		const float dx = p_camera.x - (*shown_)[0], dz = p_camera.z - (*shown_)[1];
		if (std::sqrt(dx * dx + dz * dz) <= kWindowRecentreFraction * near_side_) {
			return;
		}
	}
	ground_ = p_ground;
	for (const Page &page : near_) {
		aim(page, p_camera.x, p_camera.z, near_side_);
	}
	pending_ = std::array<float, 2>{ p_camera.x, p_camera.z };
	requested_frame_ = Engine::get_singleton()->get_frames_drawn();
}

void TerrainPageDraw::report(const Vector3 &p_camera) {
	if (user_arg_value("shots").empty()) {
		UtilityFunctions::print(to_godot(fmt("[pages] %s at (%.1f, %.1f): near %u texels at %.2f m, far %u texels at "
											 "%.2f m over %.0f m, %zu materials",
				name_.c_str(), double(p_camera.x), double(p_camera.z), render::kTerrainPageSide,
				double(stats_.near_texel), render::kTerrainPageSide, double(stats_.far_texel), double(stats_.far_side),
				materials_.size())));
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const auto read = [&](const std::array<RID, 2> &p_pages, float p_ox, float p_oz, float p_texel) {
		std::array<std::array<float, 3>, 2> out{ { { -1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f, 0.0f } } };
		const int32_t px = int32_t(std::floor((p_camera.x - p_ox) / p_texel));
		const int32_t pz = int32_t(std::floor((p_camera.z - p_oz) / p_texel));
		for (size_t i = 0; i < p_pages.size(); ++i) {
			const Ref<Image> image = rs->texture_2d_get(p_pages[i]);
			if (image.is_null() || px < 0 || pz < 0 || px >= image->get_width() || pz >= image->get_height()) {
				continue;
			}
			const Color c = image->get_pixel(px, pz);
			out[i] = i == 0 ? std::array<float, 3>{ c.r, c.g, c.b }
							: std::array<float, 3>{ c.r * 2.0f - 1.0f, c.g * 2.0f - 1.0f, c.b * 2.0f - 1.0f };
		}
		return out;
	};
	const auto near = read({ near_[0].texture, near_[1].texture }, (*shown_)[0] - 0.5f * near_side_,
			(*shown_)[1] - 0.5f * near_side_, stats_.near_texel);
	const auto far = read(far_textures_, far_centre_[0] - 0.5f * stats_.far_side,
			far_centre_[1] - 0.5f * stats_.far_side, stats_.far_texel);
	stats_.colour = near[0];
	stats_.normal = near[1];
	UtilityFunctions::print(to_godot(fmt("[pages] %s at (%.1f, %.1f): near %u texels at %.2f m, far %u texels at "
										 "%.2f m over %.0f m, %zu materials, at the camera colour (%.3f, %.3f, "
										 "%.3f) normal (%.3f, %.3f, %.3f), far page colour (%.3f, %.3f, %.3f) "
										 "normal (%.3f, %.3f, %.3f)",
			name_.c_str(), double(p_camera.x), double(p_camera.z), render::kTerrainPageSide, double(stats_.near_texel),
			render::kTerrainPageSide, double(stats_.far_texel), double(stats_.far_side), materials_.size(),
			double(near[0][0]), double(near[0][1]), double(near[0][2]), double(near[1][0]), double(near[1][1]),
			double(near[1][2]), double(far[0][0]), double(far[0][1]), double(far[0][2]), double(far[1][0]),
			double(far[1][1]), double(far[1][2]))));
}

void TerrainPageDraw::free_all() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (Page &page : near_) {
			for (RID *rid : { &page.viewport, &page.camera }) {
				if (rid->is_valid()) {
					rs->free_rid(*rid);
				}
				*rid = RID();
			}
			page.texture = RID();
		}
		for (RID *rid : { &far_textures_[0], &far_textures_[1], &instance_, &environment_, &scenario_ }) {
			if (rid->is_valid()) {
				rs->free_rid(*rid);
			}
			*rid = RID();
		}
	}
	materials_.clear();
	shown_.reset();
	pending_.reset();
	ground_.reset();
	far_pending_ = false;
	stats_ = PageDrawStats();
}

}
