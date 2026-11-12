#include "bf3/render/terrain_look.h"

#include "bf3/core/ebx.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/text.h"
#include "bf3/render/shader_database.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace bf3::render {

namespace {

constexpr std::string_view kComboSuffix = "__2d__0";
constexpr std::string_view kConstants = "externalConstants";
constexpr std::string_view kViewConstants = "viewConstants";
constexpr std::string_view kMaskPrefix = "external_worldToMask";
constexpr std::string_view kMaskSuffix = "UvTransform";

using core::to_lower_ascii;

uint32_t ceil_div(float p_extent, float p_texel) {
	return uint32_t(std::max(1.0, std::ceil(double(p_extent) / double(p_texel) - 1e-6)));
}

float finest_mask_rect(const core::res::TerrainMaskTree &p_tree) {
	float side = p_tree.bounds[2] - p_tree.bounds[0];
	for (const core::res::TerrainMaskNode &n : p_tree.nodes) {
		for (const core::res::TerrainMaskRecord &r : n.records) {
			side = std::min(side, r.rect[2] - r.rect[0]);
		}
	}
	return side;
}

bool contains(const std::vector<uint8_t> &p_set, uint8_t p_layer) {
	return std::find(p_set.begin(), p_set.end(), p_layer) != p_set.end();
}

bool subset(const std::vector<uint8_t> &p_small, const std::vector<uint8_t> &p_big) {
	return std::all_of(p_small.begin(), p_small.end(), [&](uint8_t p_l) { return contains(p_big, p_l); });
}

int32_t exact_or_superset(
		std::span<const SceneTerrainCombo> p_combos, const std::vector<uint8_t> &p_present, bool &r_exact) {
	int32_t best = -1;
	for (size_t i = 0; i < p_combos.size(); ++i) {
		if (p_combos[i].layers == p_present) {
			r_exact = true;
			return int32_t(i);
		}
		if (subset(p_present, p_combos[i].layers) &&
				(best < 0 || p_combos[i].layers.size() < p_combos[size_t(best)].layers.size())) {
			best = int32_t(i);
		}
	}
	r_exact = false;
	return best;
}

struct Region {
	core::res::TerrainRegion grid;
	std::array<float, 2> extent{};
};

Region region_for(const std::array<float, 4> &p_rect, float p_texel, uint32_t p_max_side) {
	Region r;
	r.extent = { p_rect[2] - p_rect[0], p_rect[3] - p_rect[1] };
	const float texel = std::max(p_texel, std::max(r.extent[0], r.extent[1]) / float(p_max_side));
	r.grid.origin = { p_rect[0], p_rect[1] };
	r.grid.texel = texel;
	r.grid.width = ceil_div(r.extent[0], texel);
	r.grid.height = ceil_div(r.extent[1], texel);
	return r;
}

std::array<float, 4> uv_transform(
		const core::res::TerrainRegion &p_region, float p_pixel_x, float p_pixel_y, float p_width, float p_height) {
	return { 1.0f / (p_region.texel * p_width), 1.0f / (p_region.texel * p_height),
		(p_pixel_x - p_region.origin[0] / p_region.texel) / p_width,
		(p_pixel_y - p_region.origin[1] / p_region.texel) / p_height };
}

}

std::optional<std::vector<uint8_t>> terrain_combo_layers(std::string_view p_shader, std::string_view p_asset) {
	const std::string name = to_lower_ascii(p_shader);
	const std::string prefix = to_lower_ascii(p_asset) + "__";
	const std::string suffix = to_lower_ascii(kComboSuffix);
	if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
			name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
		return std::nullopt;
	}
	const std::string_view middle =
			std::string_view(name).substr(prefix.size(), name.size() - prefix.size() - suffix.size());
	std::vector<uint8_t> layers;
	size_t at = 0;
	while (at <= middle.size()) {
		const size_t end = std::min(middle.find('_', at), middle.size());
		const std::string_view token = middle.substr(at, end - at);
		size_t digits = 0;
		while (digits < token.size() && token[digits] >= '0' && token[digits] <= '9') {
			++digits;
		}
		unsigned value = 0;
		if (digits == 0 || digits == token.size() ||
				std::from_chars(token.data(), token.data() + digits, value).ec != std::errc() || value > 255) {
			return std::nullopt;
		}
		layers.push_back(uint8_t(value));
		at = end + 1;
	}
	std::sort(layers.begin(), layers.end());
	if (std::adjacent_find(layers.begin(), layers.end()) != layers.end()) {
		return std::nullopt;
	}
	return layers;
}

std::vector<SceneTerrainCombo> plan_terrain_combos(const core::LevelMount &p_mount, const ShaderLookup &p_shaders,
		std::string_view p_asset, const std::function<int32_t(const std::string &)> &p_texture,
		std::vector<std::pair<std::string, std::string>> &r_failures) {
	std::vector<std::string> names;
	for (const std::string &name : p_mount.ebx_names()) {
		if (terrain_combo_layers(name, p_asset)) {
			names.push_back(to_lower_ascii(name));
		}
	}
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	std::map<std::string, bool> srgb;
	std::vector<SceneTerrainCombo> out;
	for (const std::string &name : names) {
		const SurfaceShader *shader = p_shaders.by_name(name);
		if (shader == nullptr) {
			r_failures.emplace_back("terrain combination " + name, "not in the shader database");
			continue;
		}
		SceneTerrainCombo combo;
		combo.shader = name;
		combo.layers = *terrain_combo_layers(name, p_asset);
		for (const auto &[slot, texture] : shader->texture_slots) {
			const std::string lower = to_lower_ascii(texture);
			auto found = srgb.find(lower);
			if (found == srgb.end()) {
				bool is_srgb = false;
				core::Result<std::pair<core::ResInfo, std::vector<uint8_t>>> res = p_mount.read_res(lower);
				core::res::DxTexture header;
				if (res && core::res::DxTexture::parse(res->second, header).ok()) {
					is_srgb = header.is_srgb();
				} else {
					r_failures.emplace_back(
							"terrain texture " + lower, res ? "header does not parse" : res.error().message);
				}
				found = srgb.emplace(lower, is_srgb).first;
			}
			combo.textures.push_back(SceneTerrainTexture{ slot, p_texture(lower), found->second });
		}
		out.push_back(std::move(combo));
	}
	return out;
}

TerrainChoiceResult choose_terrain_combo(std::span<const SceneTerrainCombo> p_combos,
		const std::vector<uint8_t> &p_present, const std::vector<uint8_t> &p_opaque) {
	bool exact = false;
	int32_t found = exact_or_superset(p_combos, p_present, exact);
	if (found >= 0) {
		return { found, exact ? TerrainChoice::Exact : TerrainChoice::Superset };
	}
	if (!p_opaque.empty()) {
		const uint8_t top = *std::max_element(p_opaque.begin(), p_opaque.end());
		std::vector<uint8_t> visible;
		for (uint8_t l : p_present) {
			if (l >= top) {
				visible.push_back(l);
			}
		}
		found = exact_or_superset(p_combos, visible, exact);
		if (found >= 0) {
			return { found, TerrainChoice::Opaque };
		}
	}
	int32_t best = -1;
	size_t best_common = 0;
	for (size_t i = 0; i < p_combos.size(); ++i) {
		size_t common = 0;
		for (uint8_t l : p_present) {
			common += contains(p_combos[i].layers, l) ? 1 : 0;
		}
		if (best < 0 || common > best_common ||
				(common == best_common && p_combos[i].layers.size() < p_combos[size_t(best)].layers.size())) {
			best = int32_t(i);
			best_common = common;
		}
	}
	return { best, TerrainChoice::Miss };
}

uint32_t TerrainLook::material_at(float p_x, float p_z) const {
	if (patch_material.empty() || !(patch > 0.0f)) {
		return 0;
	}
	const auto cell = [&](float p_v, float p_o, uint32_t p_n) {
		const double f = std::floor((double(p_v) - double(p_o)) / double(patch));
		return uint32_t(std::clamp(f, 0.0, double(p_n - 1)));
	};
	return patch_material[size_t(cell(p_z, patch_origin[1], patch_rows)) * patch_columns +
			cell(p_x, patch_origin[0], patch_columns)];
}

core::Result<std::string> terrain_shader_code(
		const PixelTranslation &p_translation, const TerrainMaterial &p_material) {
	const std::vector<TerrainTextureBinding> &p_textures = p_material.textures;
	if (p_translation.outputs < 3) {
		return core::make_error(
				"terrain combination writes " + std::to_string(p_translation.outputs) + " targets, not 3");
	}
	for (uint32_t input : p_translation.inputs) {
		if (input != 1) {
			return core::make_error("terrain combination reads input v" + std::to_string(input));
		}
	}
	std::string code = "shader_type spatial;\nrender_mode blend_mix, depth_draw_opaque, cull_back;\n";
	for (const auto &[slot, size] : p_translation.constant_buffers) {
		if (slot != p_material.view_slot) {
			code += "uniform vec4 cb" + std::to_string(slot) + "[" + std::to_string(size) + "];\n";
		}
	}
	for (uint32_t slot : p_translation.textures) {
		const auto found = std::find_if(
				p_textures.begin(), p_textures.end(), [&](const TerrainTextureBinding &b) { return b.slot == slot; });
		if (found == p_textures.end()) {
			return core::make_error("terrain texture t" + std::to_string(slot) + " has no binding");
		}
		std::string hints;
		if (found->image >= 0) {
			hints = found->image == kTerrainColourImage ? "filter_linear_mipmap, repeat_disable"
														: "filter_linear, repeat_disable";
		} else {
			hints = std::string(found->srgb ? "source_color, " : "") +
					"filter_linear_mipmap_anisotropic, repeat_enable";
		}
		code += "uniform sampler2D t" + std::to_string(slot) + " : " + hints + ";\n";
	}
	code += "void fragment() {\nvec4 v1 = vec4(UV.x, UV.y, 0.0, 0.0);\n";
	if (p_material.view_slot) {
		code += "vec4 cb" + std::to_string(*p_material.view_slot) + "[1] = vec4[1](vec4(TIME, 0.0, 0.0, 0.0));\n";
	}
	for (uint32_t o = 0; o < p_translation.outputs; ++o) {
		code += "vec4 o" + std::to_string(o) + " = vec4(0.0);\n";
	}
	code += p_translation.body;
	code += "vec3 terrain_colour = vec3(o0.y, o0.z, o2.x);\n"
			"ALBEDO = terrain_colour * terrain_colour;\n"
			"vec3 terrain_normal = normalize(vec3(o0.x, o1.z, o1.x) * 2.0 - 1.0);\n"
			"NORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(terrain_normal, 0.0))).xyz);\n"
			"ROUGHNESS = 1.0;\n}\n";
	return code;
}

core::Result<std::vector<std::vector<uint8_t>>> load_terrain_programs(
		const core::LevelMount &p_mount, std::span<const SceneTerrainCombo> p_combos) {
	std::vector<uint32_t> hashes;
	for (const SceneTerrainCombo &c : p_combos) {
		hashes.push_back(core::ebx_name_hash(to_lower_ascii(c.shader)));
	}
	std::vector<std::string> databases;
	for (const std::string &name : p_mount.res_names()) {
		const std::optional<core::ResInfo> info = p_mount.res_info(name);
		const std::string lower = to_lower_ascii(name);
		if (info && info->res_type == kShaderDatabaseResType && lower.size() >= 9 &&
				lower.compare(lower.size() - 9, 9, "/shaderdb") == 0) {
			databases.push_back(name);
		}
	}
	std::sort(databases.begin(), databases.end());
	std::vector<std::vector<uint8_t>> out(p_combos.size());
	for (const std::string &name : databases) {
		core::Result<std::pair<core::ResInfo, std::vector<uint8_t>>> res = p_mount.read_res(name);
		if (!res) {
			return core::error_at("res " + name, res.error());
		}
		core::Result<std::vector<std::vector<uint8_t>>> programs = pixel_programs(res->second, hashes);
		if (!programs) {
			return core::error_at("res " + name, programs.error());
		}
		for (size_t i = 0; i < out.size(); ++i) {
			if (out[i].empty()) {
				out[i] = std::move((*programs)[i]);
			}
		}
	}
	for (size_t i = 0; i < out.size(); ++i) {
		if (out[i].empty()) {
			return core::make_error("terrain combination " + p_combos[i].shader + " has no pixel program");
		}
	}
	return out;
}

core::Result<TerrainLook> build_terrain_look(const TerrainLookInput &p_input) {
	if (p_input.tree == nullptr || !p_input.tree->mask || p_input.heights == nullptr) {
		return core::make_error("terrain look needs the mask layer and the heights");
	}
	if (p_input.combos.empty() || p_input.programs.size() != p_input.combos.size()) {
		return core::make_error("terrain look needs its combinations and their programs");
	}
	const core::res::TerrainStreamingTree &tree = *p_input.tree;
	const core::res::TerrainMaskTree &mask = *tree.mask;
	const std::array<float, 4> &rect = p_input.region;
	if (!(rect[2] > rect[0]) || !(rect[3] > rect[1])) {
		return core::make_error("terrain look region is empty");
	}
	TerrainLook look;
	const float mask_rect = finest_mask_rect(mask);
	look.stats.data_mask_texel = mask_rect / float(mask.samples_per_side - core::res::kTerrainMaskBorder);
	const Region masks = region_for(rect, look.stats.data_mask_texel, kTerrainMaxMaskSide);
	look.stats.mask_texel = masks.grid.texel;
	std::vector<std::vector<uint8_t>> layers;
	core::res::ResError e = tree.mask_grids(masks.grid, mask.max_level(), p_input.fetch, layers);
	if (!e.ok()) {
		return core::make_error("terrain masks: " + e.message());
	}
	look.stats.layers = uint32_t(layers.size());
	std::vector<uint8_t> material_layers;
	for (const SceneTerrainCombo &c : p_input.combos) {
		for (uint8_t l : c.layers) {
			if (!contains(material_layers, l)) {
				material_layers.push_back(l);
			}
			if (l >= layers.size()) {
				return core::make_error(
						"terrain combination " + c.shader + " names layer " + std::to_string(l) + " the masks lack");
			}
		}
	}
	std::sort(material_layers.begin(), material_layers.end());

	const uint32_t split = 1u << kTerrainPatchSplits;
	look.patch = mask_rect / float(split);
	look.patch_origin = masks.grid.origin;
	const uint32_t columns_coarse = ceil_div(masks.extent[0], mask_rect);
	const uint32_t rows_coarse = ceil_div(masks.extent[1], mask_rect);
	look.patch_columns = columns_coarse * split;
	look.patch_rows = rows_coarse * split;
	look.patch_material.assign(size_t(look.patch_columns) * look.patch_rows, 0);
	std::vector<int32_t> material_of_combo(p_input.combos.size(), -1);
	std::vector<uint32_t> used_combos;
	const core::res::TerrainRegion &g = masks.grid;
	const auto choose = [&](uint32_t p_cell_x, uint32_t p_cell_z, uint32_t p_cells) {
		const auto span = [&](uint32_t p_first, uint32_t p_n) {
			const double lo = double(p_first) * double(look.patch) / double(g.texel);
			const double hi = lo + double(p_cells) * double(look.patch) / double(g.texel);
			const int64_t a = std::clamp<int64_t>(int64_t(std::floor(lo)) - 1, 0, int64_t(p_n));
			const int64_t b = std::clamp<int64_t>(int64_t(std::ceil(hi)) + 1, 0, int64_t(p_n));
			return std::pair<uint32_t, uint32_t>(uint32_t(a), uint32_t(b));
		};
		const auto [x0, x1] = span(p_cell_x, g.width);
		const auto [z0, z1] = span(p_cell_z, g.height);
		std::vector<uint8_t> present, opaque;
		for (uint8_t l : material_layers) {
			uint8_t lo = 255, hi = 0;
			for (uint32_t z = z0; z < z1; ++z) {
				for (uint32_t x = x0; x < x1; ++x) {
					const uint8_t v = layers[l][size_t(z) * g.width + x];
					lo = std::min(lo, v);
					hi = std::max(hi, v);
				}
			}
			if (hi > 0) {
				present.push_back(l);
			}
			if (lo == core::res::kTerrainFullMask && z1 > z0 && x1 > x0) {
				opaque.push_back(l);
			}
		}
		return std::pair<TerrainChoiceResult, std::vector<uint8_t>>(
				choose_terrain_combo(p_input.combos, present, opaque), present);
	};
	const std::function<core::Status(uint32_t, uint32_t, uint32_t)> place = [&](uint32_t p_x, uint32_t p_z,
																					uint32_t p_cells) -> core::Status {
		const auto [choice, present] = choose(p_x, p_z, p_cells);
		if (choice.combo < 0) {
			return core::make_error("terrain look has no combination for a patch");
		}
		if (choice.kind == TerrainChoice::Miss && p_cells > 1) {
			const uint32_t half = p_cells / 2;
			for (uint32_t q = 0; q < 4; ++q) {
				BF3_TRY(place(p_x + (q & 1u) * half, p_z + (q >> 1) * half, half));
			}
			return core::Status::success();
		}
		++look.stats.patches;
		look.stats.exact += choice.kind == TerrainChoice::Exact ? 1u : 0u;
		look.stats.superset += choice.kind == TerrainChoice::Superset ? 1u : 0u;
		look.stats.opaque += choice.kind == TerrainChoice::Opaque ? 1u : 0u;
		look.stats.misses += choice.kind == TerrainChoice::Miss ? 1u : 0u;
		if (choice.kind == TerrainChoice::Miss && look.stats.missed.size() < kTerrainMissedSets &&
				std::find(look.stats.missed.begin(), look.stats.missed.end(), present) == look.stats.missed.end()) {
			look.stats.missed.push_back(present);
		}
		int32_t &material = material_of_combo[size_t(choice.combo)];
		if (material < 0) {
			material = int32_t(used_combos.size());
			used_combos.push_back(uint32_t(choice.combo));
		}
		for (uint32_t z = p_z; z < p_z + p_cells; ++z) {
			for (uint32_t x = p_x; x < p_x + p_cells; ++x) {
				look.patch_material[size_t(z) * look.patch_columns + x] = uint16_t(material);
			}
		}
		return core::Status::success();
	};
	for (uint32_t pr = 0; pr < rows_coarse; ++pr) {
		for (uint32_t pc = 0; pc < columns_coarse; ++pc) {
			const core::Status placed = place(pc * split, pr * split, split);
			if (!placed) {
				return placed.error();
			}
		}
	}

	const uint32_t tiles = uint32_t(layers.size());
	const uint32_t columns = std::max<uint32_t>(1, uint32_t(std::ceil(std::sqrt(double(tiles)))));
	const uint32_t rows = (tiles + columns - 1) / columns;
	const uint32_t tile_w = g.width + 2, tile_h = g.height + 2;
	TerrainImage atlas;
	atlas.format = TerrainImageFormat::R8;
	atlas.width = columns * tile_w;
	atlas.height = rows * tile_h;
	atlas.data.assign(size_t(atlas.width) * atlas.height, 0);
	for (uint32_t t = 0; t < tiles; ++t) {
		const uint32_t ox = (t % columns) * tile_w, oy = (t / columns) * tile_h;
		for (uint32_t y = 0; y < tile_h; ++y) {
			const uint32_t sy = std::min(g.height - 1, y == 0 ? 0 : y - 1);
			for (uint32_t x = 0; x < tile_w; ++x) {
				const uint32_t sx = std::min(g.width - 1, x == 0 ? 0 : x - 1);
				atlas.data[size_t(oy + y) * atlas.width + ox + x] = layers[t][size_t(sy) * g.width + sx];
			}
		}
	}
	const float atlas_w = float(atlas.width), atlas_h = float(atlas.height);
	layers.clear();

	TerrainImage colour;
	colour.format = TerrainImageFormat::Rgba8;
	Region colours = region_for(rect, std::max(rect[2] - rect[0], rect[3] - rect[1]), 1);
	if (tree.colour) {
		const core::res::TerrainColourTree &colour_tree = *tree.colour;
		const float colour_node =
				(colour_tree.bounds[2] - colour_tree.bounds[0]) / float(uint32_t(1) << colour_tree.max_level());
		look.stats.data_colour_texel =
				colour_node / float(colour_tree.texels_per_side - core::res::kTerrainColourBorder);
		colours = region_for(rect, look.stats.data_colour_texel, kTerrainMaxColourSide);
		colour.mipmaps = true;
		e = tree.colour_grid(colours.grid, colour_tree.max_level(), p_input.fetch, colour.data);
		if (!e.ok()) {
			return core::make_error("terrain colour: " + e.message());
		}
	} else {
		colour.data.assign(size_t(colours.grid.width) * colours.grid.height * 4, kTerrainNeutralColour);
	}
	look.stats.colour_texel = colours.grid.texel;
	colour.width = colours.grid.width;
	colour.height = colours.grid.height;

	const core::res::HeightGrid &hg = *p_input.heights;
	const auto grid_index = [&](float p_v, float p_o, float p_s, size_t p_n, bool p_up) {
		const double f = (double(p_v) - double(p_o)) / double(p_s);
		const double r = p_up ? std::ceil(f) + 1.0 : std::floor(f) - 1.0;
		return size_t(std::clamp(r, 0.0, double(p_n - 1)));
	};
	const size_t hx0 = grid_index(rect[0], hg.origin_xz[0], hg.sample_spacing[0], hg.width, false);
	const size_t hx1 = grid_index(rect[2], hg.origin_xz[0], hg.sample_spacing[0], hg.width, true);
	const size_t hz0 = grid_index(rect[1], hg.origin_xz[1], hg.sample_spacing[1], hg.height, false);
	const size_t hz1 = grid_index(rect[3], hg.origin_xz[1], hg.sample_spacing[1], hg.height, true);
	if (hx1 <= hx0 || hz1 <= hz0) {
		return core::make_error("terrain heights do not cover the look region");
	}
	TerrainImage height;
	height.format = TerrainImageFormat::R32f;
	height.width = uint32_t(hx1 - hx0 + 1);
	height.height = uint32_t(hz1 - hz0 + 1);
	height.data.resize(size_t(height.width) * height.height * 4);
	for (size_t z = 0; z < height.height; ++z) {
		for (size_t x = 0; x < height.width; ++x) {
			const float h = hg.at(hx0 + x, hz0 + z);
			std::memcpy(height.data.data() + (z * height.width + x) * 4, &h, 4);
		}
	}
	const float hox = hg.origin_xz[0] + float(hx0) * hg.sample_spacing[0];
	const float hoz = hg.origin_xz[1] + float(hz0) * hg.sample_spacing[1];
	const std::array<float, 4> height_uv = { 1.0f / (hg.sample_spacing[0] * float(height.width)),
		1.0f / (hg.sample_spacing[1] * float(height.height)), (0.5f - hox / hg.sample_spacing[0]) / float(height.width),
		(0.5f - hoz / hg.sample_spacing[1]) / float(height.height) };
	const std::array<float, 4> height_pixel = { 1.0f / float(height.width), 1.0f / float(height.height),
		1.0f / hg.sample_spacing[1], 1.0f / hg.sample_spacing[0] };

	TerrainImage dynamic;
	dynamic.format = TerrainImageFormat::R8;
	dynamic.width = dynamic.height = 1;
	dynamic.data.assign(1, 0);

	for (const TerrainImage *img : { &atlas, &colour, &height, &dynamic }) {
		look.stats.image_bytes += img->data.size();
	}
	look.images = { std::move(atlas), std::move(colour), std::move(height), std::move(dynamic) };

	const std::array<float, 4> colour_uv =
			uv_transform(colours.grid, 0.0f, 0.0f, float(colours.grid.width), float(colours.grid.height));
	for (uint32_t combo_index : used_combos) {
		const SceneTerrainCombo &combo = p_input.combos[combo_index];
		const std::string where = "terrain combination " + combo.shader;
		core::Result<dxbc::Program> program = dxbc::parse(p_input.programs[combo_index]);
		if (!program) {
			return core::error_at(where, program.error());
		}
		core::Result<PixelTranslation> translation = translate_pixel_program(*program);
		if (!translation) {
			return core::error_at(where, translation.error());
		}
		look.stats.instructions += translation->instructions;
		TerrainMaterial material;
		material.combo = combo_index;
		for (uint32_t slot : translation->textures) {
			TerrainTextureBinding b;
			b.slot = slot;
			const dxbc::Binding *bound = program->binding(dxbc::BindingType::Texture, slot);
			const std::string name = bound == nullptr ? std::string() : bound->name;
			if (name == "texture_maskMap") {
				b.image = kTerrainMaskAtlas;
			} else if (name == "texture_globalColorMap") {
				b.image = kTerrainColourImage;
			} else if (name == "texture_heightfieldAtlas") {
				b.image = kTerrainHeightImage;
			} else if (name == "texture_dynamicMask") {
				b.image = kTerrainDynamicMask;
			} else {
				const auto data = std::find_if(combo.textures.begin(), combo.textures.end(),
						[&](const SceneTerrainTexture &t) { return t.slot == slot; });
				if (data == combo.textures.end() || data->texture < 0) {
					return core::make_error(where + ": texture t" + std::to_string(slot) + " (" + name +
							") is not in the shader database");
				}
				b.texture = data->texture;
				b.srgb = data->srgb;
			}
			material.textures.push_back(b);
		}
		const dxbc::Binding *constants = program->binding(dxbc::BindingType::ConstantBuffer, kConstants);
		const dxbc::Binding *view = program->binding(dxbc::BindingType::ConstantBuffer, kViewConstants);
		const dxbc::Variable *time = program->variable(kViewConstants, "time");
		for (const auto &[slot, size] : translation->constant_buffers) {
			if (view != nullptr && slot == view->bind_point && size == 1 && time != nullptr && time->offset == 0) {
				material.view_slot = slot;
				continue;
			}
			if (constants == nullptr || slot != constants->bind_point) {
				return core::make_error(
						where + ": reads constant buffer cb" + std::to_string(slot) + " beyond the view time");
			}
			material.constant_slot = slot;
			material.constants.assign(size, std::array<float, 4>{});
		}
		if (constants != nullptr) {
			for (const dxbc::ConstantBuffer &cb : program->constant_buffers) {
				if (cb.name != kConstants) {
					continue;
				}
				for (const dxbc::Variable &v : cb.variables) {
					std::array<float, 4> value{};
					if (v.name == "external_clipToWorldSpaceXzTransform") {
						value = { 1.0f, 1.0f, 0.0f, 0.0f };
					} else if (v.name == "external_dynamicMaskUvScale") {
						value = { 1.0f, 1.0f, 0.0f, 0.0f };
					} else if (v.name == "external_worldToGlobalColorUvTransform") {
						value = colour_uv;
					} else if (v.name == "external_worldToHeightfieldUvTransform") {
						value = height_uv;
					} else if (v.name == "external_heightfieldPixelSizeAndAspect") {
						value = height_pixel;
					} else if (v.name == "external_heightmapWorldSizeY") {
						value = { 1.0f, 0.0f, 0.0f, 0.0f };
					} else if (v.name.starts_with(kMaskPrefix) && v.name.ends_with(kMaskSuffix)) {
						const std::string_view digits = std::string_view(v.name).substr(
								kMaskPrefix.size(), v.name.size() - kMaskPrefix.size() - kMaskSuffix.size());
						unsigned n = 0;
						std::vector<uint8_t> masked;
						for (uint8_t l : combo.layers) {
							if (l != 0) {
								masked.push_back(l);
							}
						}
						if (std::from_chars(digits.data(), digits.data() + digits.size(), n).ec != std::errc() ||
								n >= masked.size()) {
							return core::make_error(where + ": " + v.name + " has no masked layer");
						}
						const uint32_t tile = masked[n];
						value = uv_transform(g, float((tile % columns) * tile_w + 1),
								float((tile / columns) * tile_h + 1), atlas_w, atlas_h);
					} else {
						return core::make_error(where + ": unknown constant " + v.name);
					}
					const uint32_t reg = v.offset / 16;
					const uint32_t first = (v.offset % 16) / 4;
					for (uint32_t k = 0; k < v.size / 4 && first + k < 4 && reg < material.constants.size(); ++k) {
						material.constants[reg][first + k] = value[k];
					}
				}
			}
		}
		core::Result<std::string> code = terrain_shader_code(*translation, material);
		if (!code) {
			return core::error_at(where, code.error());
		}
		material.code = std::move(*code);
		look.materials.push_back(std::move(material));
	}
	return look;
}

}
