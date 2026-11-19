#include "bf3/render/terrain_detail.h"

#include <algorithm>
#include <cmath>

namespace bf3::render {

namespace {

constexpr std::string_view kMaskFunction = "terrain_detail_mask";
constexpr std::string_view kColourFunction = "terrain_detail_colour";

uint32_t texels_over(double p_extent, float p_texel) {
	return uint32_t(std::max(0.0, std::ceil(p_extent / double(p_texel) - 1e-6)));
}

core::res::TerrainRegion window_for(const std::array<float, 4> &p_region, const std::array<float, 2> &p_grid,
		float p_texel, double p_side, float p_x, float p_z) {
	core::res::TerrainRegion out;
	out.texel = p_texel;
	const auto axis = [&](float p_centre, float p_lo, float p_hi, float p_snap, float &r_origin, uint32_t &r_count) {
		const double extent = double(p_hi) - double(p_lo);
		const double side = std::min(p_side, extent);
		double origin = double(p_centre) - side * 0.5;
		origin = std::clamp(origin, double(p_lo), double(p_hi) - side);
		origin = double(p_snap) + std::ceil((origin - double(p_snap)) / double(p_texel) - 1e-6) * double(p_texel);
		r_origin = float(origin);
		r_count = std::min(kTerrainDetailSide, texels_over(double(p_hi) - origin, p_texel));
		r_count = std::min(r_count, texels_over(side, p_texel));
	};
	axis(p_x, p_region[0], p_region[2], p_grid[0], out.origin[0], out.width);
	axis(p_z, p_region[1], p_region[3], p_grid[1], out.origin[1], out.height);
	return out;
}

double window_side(const TerrainDetailLayout &p_layout) {
	if (p_layout.data_mask_texel > 0.0f && p_layout.data_mask_texel < p_layout.mask.texel) {
		return double(kTerrainDetailSide) * double(p_layout.data_mask_texel);
	}
	return double(kTerrainDetailSide) * double(p_layout.data_colour_texel);
}

bool colour_gains(const TerrainDetailLayout &p_layout) {
	return p_layout.colour && p_layout.data_colour_texel > 0.0f && p_layout.data_colour_texel < p_layout.colour->texel;
}

bool mask_gains(const TerrainDetailLayout &p_layout) {
	return p_layout.data_mask_texel > 0.0f && p_layout.data_mask_texel < p_layout.mask.texel;
}

size_t count_of(const std::string &p_text, const std::string &p_what) {
	size_t count = 0;
	for (size_t at = p_text.find(p_what); at != std::string::npos; at = p_text.find(p_what, at + p_what.size())) {
		++count;
	}
	return count;
}

core::Status replace_samples(std::string &r_body, uint32_t p_slot, std::string_view p_function) {
	const std::string name = "t" + std::to_string(p_slot);
	const std::string from = "texture(" + name + ", ";
	const size_t samples = count_of(r_body, from);
	if (samples == 0 || samples != count_of(r_body, "(" + name + ",")) {
		return core::make_error("terrain detail: " + name + " is read other than by texture()");
	}
	const std::string to = std::string(p_function) + "(" + name + ", ";
	for (size_t at = r_body.find(from); at != std::string::npos; at = r_body.find(from, at + to.size())) {
		r_body.replace(at, from.size(), to);
	}
	return core::Status::success();
}

}

core::res::TerrainChunkFetch TerrainDetailSource::fetch() const {
	return [this](const core::res::ChunkId &p_id) -> std::optional<std::vector<uint8_t>> {
		const auto found = chunks.find(p_id.bytes);
		if (found == chunks.end()) {
			return std::nullopt;
		}
		return found->second;
	};
}

uint64_t TerrainDetailSource::bytes() const {
	uint64_t total = 0;
	for (const auto &[id, bytes] : chunks) {
		total += bytes.size();
	}
	return total;
}

std::array<float, 4> TerrainDetail::mask_window() const {
	if (mask.width == 0 || mask.height == 0) {
		return {};
	}
	return { mask.origin[0], mask.origin[1], 1.0f / mask.texel, 0.0f };
}

std::array<float, 4> TerrainDetail::mask_size() const {
	return { float(mask.width), float(mask.height), float(mask_atlas_width), float(mask_atlas_height) };
}

std::array<float, 4> TerrainDetail::colour_window() const {
	if (colour.width == 0 || colour.height == 0) {
		return {};
	}
	return { colour.origin[0], colour.origin[1], 1.0f / colour.texel, 0.0f };
}

std::array<float, 4> TerrainDetail::colour_size() const {
	return { float(colour.width), float(colour.height), 0.0f, 0.0f };
}

bool terrain_detail_gains(const TerrainDetailLayout &p_layout) {
	return mask_gains(p_layout) || colour_gains(p_layout);
}

bool terrain_detail_covers(const core::res::TerrainRegion &p_region, float p_x, float p_z) {
	if (p_region.width == 0 || p_region.height == 0) {
		return false;
	}
	const double fx = (double(p_x) - double(p_region.origin[0])) / double(p_region.texel);
	const double fz = (double(p_z) - double(p_region.origin[1])) / double(p_region.texel);
	return fx >= 0.0 && fz >= 0.0 && fx <= double(p_region.width) && fz <= double(p_region.height);
}

core::Result<TerrainDetail> build_terrain_detail(
		const TerrainDetailSource &p_source, const TerrainDetailLayout &p_layout, float p_x, float p_z) {
	if (!p_source.tree.mask) {
		return core::make_error("terrain detail needs the mask layer");
	}
	TerrainDetail out;
	out.centre = { p_x, p_z };
	const core::res::TerrainChunkFetch fetch = p_source.fetch();
	const double side = window_side(p_layout);
	if (mask_gains(p_layout)) {
		out.mask = window_for(p_layout.region, p_layout.mask_grid_origin, p_layout.data_mask_texel, side, p_x, p_z);
		std::vector<std::vector<uint8_t>> layers;
		const core::res::ResError e = p_source.tree.mask_grids(out.mask, p_layout.mask_depth, fetch, layers);
		if (!e.ok()) {
			return core::make_error("terrain detail masks: " + e.message());
		}
		if (layers.size() != p_layout.mask_tiles) {
			return core::make_error("terrain detail masks: " + std::to_string(layers.size()) +
					" layers, the look has " + std::to_string(p_layout.mask_tiles));
		}
		const uint32_t columns = p_layout.mask_columns;
		const uint32_t tile_w = out.mask.width + 2, tile_h = out.mask.height + 2;
		out.mask_atlas_width = columns * tile_w;
		out.mask_atlas_height = ((p_layout.mask_tiles + columns - 1) / columns) * tile_h;
		out.mask_atlas.assign(size_t(out.mask_atlas_width) * out.mask_atlas_height, 0);
		for (uint32_t t = 0; t < p_layout.mask_tiles; ++t) {
			const uint32_t ox = (t % columns) * tile_w, oy = (t / columns) * tile_h;
			for (uint32_t y = 0; y < tile_h; ++y) {
				const uint32_t sy = std::min(out.mask.height - 1, y == 0 ? 0 : y - 1);
				for (uint32_t x = 0; x < tile_w; ++x) {
					const uint32_t sx = std::min(out.mask.width - 1, x == 0 ? 0 : x - 1);
					out.mask_atlas[size_t(oy + y) * out.mask_atlas_width + ox + x] =
							layers[t][size_t(sy) * out.mask.width + sx];
				}
			}
		}
	}
	if (colour_gains(p_layout) && p_source.tree.colour) {
		out.colour =
				window_for(p_layout.region, p_layout.colour_grid_origin, p_layout.data_colour_texel, side, p_x, p_z);
		const core::res::ResError e =
				p_source.tree.colour_grid(out.colour, p_layout.colour_depth, fetch, out.colour_rgba);
		if (!e.ok()) {
			return core::make_error("terrain detail colour: " + e.message());
		}
	}
	return out;
}

float terrain_mask_texel_at(const TerrainDetailLayout &p_layout, const TerrainDetail *p_detail, float p_x, float p_z) {
	if (p_detail != nullptr && terrain_detail_covers(p_detail->mask, p_x, p_z)) {
		return p_detail->mask.texel;
	}
	return p_layout.mask.texel;
}

float terrain_colour_texel_at(
		const TerrainDetailLayout &p_layout, const TerrainDetail *p_detail, float p_x, float p_z) {
	if (p_detail != nullptr && terrain_detail_covers(p_detail->colour, p_x, p_z)) {
		return p_detail->colour.texel;
	}
	return p_layout.colour ? p_layout.colour->texel : 0.0f;
}

core::Result<TerrainDetailCode> terrain_detail_code(
		std::span<const uint32_t> p_mask_slots, std::span<const uint32_t> p_colour_slots, std::string p_body) {
	TerrainDetailCode out;
	for (uint32_t slot : p_mask_slots) {
		BF3_TRY(replace_samples(p_body, slot, kMaskFunction));
	}
	for (uint32_t slot : p_colour_slots) {
		BF3_TRY(replace_samples(p_body, slot, kColourFunction));
	}
	if (!p_mask_slots.empty()) {
		out.functions += R"(uniform sampler2D detail_mask : filter_linear, repeat_disable;
uniform vec4 detail_mask_window = vec4(0.0);
uniform vec4 detail_mask_size = vec4(0.0);
uniform vec4 detail_mask_coarse = vec4(0.0);
uniform vec4 detail_mask_coarse_atlas = vec4(1.0);
vec4 terrain_detail_mask(sampler2D p_coarse, vec2 p_uv) {
	vec2 px = p_uv * detail_mask_coarse_atlas.xy;
	vec2 tile = floor(px / detail_mask_coarse_atlas.zw);
	vec2 world = detail_mask_coarse.xy + (px - tile * detail_mask_coarse_atlas.zw - 1.0) * detail_mask_coarse.z;
	vec2 f = (world - detail_mask_window.xy) * detail_mask_window.z;
	if (detail_mask_window.z > 0.0 && f.x >= 0.0 && f.y >= 0.0 && f.x <= detail_mask_size.x && f.y <= detail_mask_size.y) {
		return texture(detail_mask, (tile * (detail_mask_size.xy + 2.0) + 1.0 + f) / detail_mask_size.zw);
	}
	return texture(p_coarse, p_uv);
}
)";
	}
	if (!p_colour_slots.empty()) {
		out.functions += R"(uniform sampler2D detail_colour : filter_linear_mipmap, repeat_disable;
uniform vec4 detail_colour_window = vec4(0.0);
uniform vec4 detail_colour_size = vec4(0.0);
uniform vec4 detail_colour_coarse = vec4(0.0);
uniform vec4 detail_colour_coarse_size = vec4(1.0);
vec4 terrain_detail_colour(sampler2D p_coarse, vec2 p_uv) {
	vec4 coarse = texture(p_coarse, p_uv);
	vec2 world = detail_colour_coarse.xy + p_uv * detail_colour_coarse_size.xy * detail_colour_coarse.z;
	vec2 f = (world - detail_colour_window.xy) * detail_colour_window.z;
	vec4 fine = texture(detail_colour, f / max(detail_colour_size.xy, vec2(1.0)));
	bool inside = detail_colour_window.z > 0.0 && f.x >= 0.0 && f.y >= 0.0 && f.x <= detail_colour_size.x && f.y <= detail_colour_size.y;
	return inside ? fine : coarse;
}
)";
	}
	out.body = std::move(p_body);
	return out;
}

}
