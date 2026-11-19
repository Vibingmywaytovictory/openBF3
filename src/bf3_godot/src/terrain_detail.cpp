#include "terrain_detail.h"

#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/format.h"
#include "godot_util.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace bf3::godot_side {

using namespace godot;
namespace render = bf3::render;
using bf3::core::fmt;

namespace {

float window_side(const render::TerrainDetail &p_detail) {
	const float mask = float(std::max(p_detail.mask.width, p_detail.mask.height)) * p_detail.mask.texel;
	const float colour = float(std::max(p_detail.colour.width, p_detail.colour.height)) * p_detail.colour.texel;
	return std::max(mask, colour);
}

void replace_texture(RID &r_texture, const Ref<Image> &p_image) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (p_image.is_null() || p_image->is_empty()) {
		return;
	}
	if (r_texture.is_valid()) {
		const Ref<Image> old = rs->texture_2d_get(r_texture);
		if (old.is_valid() && old->get_size() == p_image->get_size() && old->get_format() == p_image->get_format() &&
				old->has_mipmaps() == p_image->has_mipmaps()) {
			rs->texture_2d_update(r_texture, p_image, 0);
			return;
		}
		rs->free_rid(r_texture);
	}
	r_texture = rs->texture_2d_create(p_image);
}

}

Vector4 vec4_of(const std::array<float, 4> &p_v) {
	return Vector4(p_v[0], p_v[1], p_v[2], p_v[3]);
}

Ref<Image> image_from_bytes(uint32_t p_width, uint32_t p_height, Image::Format p_format,
		const std::vector<uint8_t> &p_data, bool p_mipmaps) {
	if (p_width == 0 || p_height == 0 || p_data.empty()) {
		return Ref<Image>();
	}
	PackedByteArray bytes;
	bytes.resize(int64_t(p_data.size()));
	std::memcpy(bytes.ptrw(), p_data.data(), p_data.size());
	Ref<Image> image = Image::create_from_data(int32_t(p_width), int32_t(p_height), false, p_format, bytes);
	if (image.is_valid() && !image->is_empty() && p_mipmaps) {
		image->generate_mipmaps();
	}
	return image;
}

TerrainDetailDraw::~TerrainDetailDraw() {
	free_all();
}

void TerrainDetailDraw::begin(std::string p_name, std::shared_ptr<const render::TerrainDetailSource> p_source,
		const render::TerrainDetailLayout &p_layout, std::vector<RID> p_materials, const Transform3D &p_terrain) {
	free_all();
	if (p_source == nullptr || p_materials.empty()) {
		return;
	}
	name_ = std::move(p_name);
	source_ = std::move(p_source);
	layout_ = p_layout;
	materials_ = std::move(p_materials);
	terrain_ = p_terrain;
	jobs_ = std::make_unique<bf3::core::JobQueue>("terrain-detail");
	UtilityFunctions::print(to_godot(fmt("[detail] %s: masks %.2f m (data %.2f m), colour %.2f m (data %.2f m), "
										 "%zu materials, %.1f MiB of terrain chunks kept",
			name_.c_str(), double(layout_.mask.texel), double(layout_.data_mask_texel),
			double(layout_.colour ? layout_.colour->texel : 0.0f), double(layout_.data_colour_texel), materials_.size(),
			double(source_->bytes()) / 1048576.0)));
}

void TerrainDetailDraw::free_all() {
	if (jobs_ != nullptr) {
		jobs_->stop();
		jobs_.reset();
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (RID *rid : { &mask_texture_, &colour_texture_ }) {
			if (rid->is_valid()) {
				rs->free_rid(*rid);
			}
			*rid = RID();
		}
	}
	{
		std::lock_guard lock(*ready_mutex_);
		ready_->reset();
	}
	building_ = false;
	shown_.reset();
	source_.reset();
	materials_.clear();
	stats_ = DetailDrawStats();
}

void TerrainDetailDraw::update(const Vector3 &p_camera) {
	if (source_ == nullptr) {
		return;
	}
	const Vector3 local = terrain_.affine_inverse().xform(p_camera);
	std::optional<Built> built;
	{
		std::lock_guard lock(*ready_mutex_);
		if (ready_->has_value()) {
			built = std::move(**ready_);
			ready_->reset();
		}
	}
	if (built) {
		building_ = false;
		apply(*built);
		report(local);
	}
	if (building_) {
		return;
	}
	if (shown_) {
		const float dx = local.x - shown_->centre[0], dz = local.z - shown_->centre[1];
		if (std::sqrt(dx * dx + dz * dz) <= kWindowRecentreFraction * window_side(*shown_)) {
			return;
		}
	} else if (stats_.failures > 0) {
		return;
	}
	const std::shared_ptr<const render::TerrainDetailSource> source = source_;
	const render::TerrainDetailLayout layout = layout_;
	const std::shared_ptr<std::mutex> mutex = ready_mutex_;
	const std::shared_ptr<std::optional<Built>> ready = ready_;
	const float x = local.x, z = local.z;
	building_ = jobs_->post([source, layout, mutex, ready, x, z]() {
		const auto start = std::chrono::steady_clock::now();
		Built out;
		core::Result<render::TerrainDetail> detail = render::build_terrain_detail(*source, layout, x, z);
		if (!detail) {
			out.error = detail.error().message;
		} else {
			out.detail = std::move(*detail);
			out.mask = image_from_bytes(out.detail.mask_atlas_width, out.detail.mask_atlas_height, Image::FORMAT_R8,
					out.detail.mask_atlas, false);
			out.colour = image_from_bytes(out.detail.colour.width, out.detail.colour.height, Image::FORMAT_RGBA8,
					out.detail.colour_rgba, true);
			out.detail.mask_atlas.clear();
			out.detail.mask_atlas.shrink_to_fit();
			out.detail.colour_rgba.clear();
			out.detail.colour_rgba.shrink_to_fit();
		}
		out.seconds = core::seconds_since(start);
		std::lock_guard lock(*mutex);
		*ready = std::move(out);
	});
}

void TerrainDetailDraw::apply(Built &r_built) {
	stats_.build_seconds += r_built.seconds;
	if (!r_built.error.empty()) {
		++stats_.failures;
		stats_.last_error = r_built.error;
		UtilityFunctions::print(to_godot(fmt("[detail] %s: %s", name_.c_str(), r_built.error.c_str())));
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	replace_texture(mask_texture_, r_built.mask);
	replace_texture(colour_texture_, r_built.colour);
	const render::TerrainDetail &d = r_built.detail;
	const bool has_mask = mask_texture_.is_valid() && d.mask.width > 0;
	const bool has_colour = colour_texture_.is_valid() && d.colour.width > 0;
	for (const RID &material : materials_) {
		if (has_mask) {
			rs->material_set_param(material, "detail_mask", mask_texture_);
		}
		rs->material_set_param(material, "detail_mask_window", has_mask ? vec4_of(d.mask_window()) : Vector4());
		rs->material_set_param(material, "detail_mask_size", vec4_of(d.mask_size()));
		if (has_colour) {
			rs->material_set_param(material, "detail_colour", colour_texture_);
		}
		rs->material_set_param(material, "detail_colour_window", has_colour ? vec4_of(d.colour_window()) : Vector4());
		rs->material_set_param(material, "detail_colour_size", vec4_of(d.colour_size()));
	}
	shown_ = std::move(r_built.detail);
	++stats_.windows;
}

void TerrainDetailDraw::report(const Vector3 &p_local) {
	const render::TerrainDetail *shown = shown_ ? &*shown_ : nullptr;
	stats_.mask_texel = render::terrain_mask_texel_at(layout_, shown, p_local.x, p_local.z);
	stats_.colour_texel = render::terrain_colour_texel_at(layout_, shown, p_local.x, p_local.z);
	UtilityFunctions::print(to_godot(fmt("[detail] %s at (%.1f, %.1f): window %llu, masks %.2f m (data %.2f m), "
										 "colour %.2f m (data %.2f m) at the camera, built in %.3f s",
			name_.c_str(), double(p_local.x), double(p_local.z), static_cast<unsigned long long>(stats_.windows),
			double(stats_.mask_texel), double(layout_.data_mask_texel), double(stats_.colour_texel),
			double(layout_.data_colour_texel), stats_.build_seconds)));
}

}
