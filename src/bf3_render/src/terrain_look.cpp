#include "bf3/render/terrain_look.h"

#include "bf3/render/light_pass.h"

#include "bf3/core/ebx.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/text.h"
#include "bf3/render/dxbc.h"
#include "bf3/render/shader_database.h"
#include "bf3/render/terrain_page.h"
#include "bf3/render/terrain_program.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace bf3::render {

namespace {

constexpr std::string_view kComboSuffix = "__2d__0";
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

SceneTerrainTexture plan_terrain_texture(const core::LevelMount &p_mount, uint32_t p_slot, const std::string &p_texture,
		const std::function<int32_t(const std::string &)> &p_register, std::map<std::string, bool> &r_srgb,
		std::vector<std::pair<std::string, std::string>> &r_failures) {
	const std::string lower = to_lower_ascii(p_texture);
	auto found = r_srgb.find(lower);
	if (found == r_srgb.end()) {
		bool is_srgb = false;
		core::Result<std::pair<core::ResInfo, std::vector<uint8_t>>> res = p_mount.read_res(lower);
		core::res::DxTexture header;
		if (res && core::res::DxTexture::parse(res->second, header).ok()) {
			is_srgb = header.is_srgb();
		} else {
			r_failures.emplace_back("terrain texture " + lower, res ? "header does not parse" : res.error().message);
		}
		found = r_srgb.emplace(lower, is_srgb).first;
	}
	return SceneTerrainTexture{ p_slot, p_register(lower), found->second };
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
		// STAND-IN: the first solution with a pixel program stands for the one the engine picks. Settle: the solution choice read.
		for (const ShaderSolution &s : shader->solutions) {
			if (s.pixel_program) {
				combo.program = s.pixel_program;
				break;
			}
		}
		for (const auto &[slot, texture] : shader->texture_slots) {
			combo.textures.push_back(plan_terrain_texture(p_mount, slot, texture, p_texture, srgb, r_failures));
		}
		out.push_back(std::move(combo));
	}
	return out;
}

namespace {

core::Result<TerrainLane> trace_surface_lane(
		const dxbc::Program &p_program, size_t p_end, uint8_t p_type, uint32_t p_reg, uint8_t p_lane) {
	for (size_t i = p_end; i-- > 0;) {
		const dxbc::Instruction &ins = p_program.code[i];
		if (ins.operands.empty() || ins.operands[0].type != p_type || ins.operands[0].index[0] != p_reg ||
				((ins.operands[0].write_mask() >> p_lane) & 1u) == 0) {
			continue;
		}
		const auto source = [&](size_t p_operand) -> core::Result<TerrainLane> {
			const dxbc::Operand &s = ins.operands[p_operand];
			if (s.is(dxbc::OperandType::Immediate32)) {
				TerrainLane out;
				out.literal = std::bit_cast<float>(s.imm[s.components == 1 ? 0 : p_lane]);
				return out;
			}
			if (!s.is(dxbc::OperandType::Temp)) {
				return core::make_error("reads " + dxbc::format_instruction(ins));
			}
			return trace_surface_lane(p_program, i, uint8_t(dxbc::OperandType::Temp), s.index[0], s.lane(p_lane));
		};
		if (ins.is(dxbc::Op::Mov) && ins.operands.size() == 2 && !ins.operands[1].negate) {
			return source(1);
		}
		if ((ins.is(dxbc::Op::Sample) || ins.is(dxbc::Op::SampleL) || ins.is(dxbc::Op::SampleD)) &&
				ins.operands.size() > 2) {
			const dxbc::Binding *b = p_program.binding(dxbc::BindingType::Texture, ins.operands[2].index[0]);
			const int32_t atlas = b != nullptr ? terrain_page_atlas(b->name) : -1;
			if (atlas < 0) {
				return core::make_error("samples what is not a terrain atlas: " + dxbc::format_instruction(ins));
			}
			TerrainLane out;
			out.atlas = uint8_t(atlas);
			out.lane = ins.operands[2].lane(p_lane);
			return out;
		}
		const bool affine = ins.is(dxbc::Op::Mad) && ins.operands.size() == 4 &&
				ins.operands[2].is(dxbc::OperandType::Immediate32) &&
				ins.operands[3].is(dxbc::OperandType::Immediate32) && !ins.operands[1].negate;
		if (affine) {
			core::Result<TerrainLane> a = source(1);
			core::Result<TerrainLane> k = source(2);
			core::Result<TerrainLane> b = source(3);
			if (!a) {
				return a.error();
			}
			if (a->literal) {
				a->literal = *a->literal * *k->literal + *b->literal;
			} else {
				a->scale *= *k->literal;
				a->bias = a->bias * *k->literal + *b->literal;
			}
			return *a;
		}
		const bool add =
				ins.is(dxbc::Op::Add) && ins.operands.size() == 3 && ins.operands[1].negate != ins.operands[2].negate;
		const bool lerp = ins.is(dxbc::Op::Mad) && ins.operands.size() == 4;
		if (add || lerp) {
			core::Result<TerrainLane> a = source(add ? 1 : 2);
			core::Result<TerrainLane> b = source(add ? 2 : 3);
			if (!a || !b) {
				return a ? b.error() : a.error();
			}
			if (*a == *b) {
				return *a;
			}
		}
		return core::make_error("is not one atlas lane or literal: " + dxbc::format_instruction(ins));
	}
	return core::make_error("reads a value no instruction writes");
}

}

core::Result<TerrainSurfaceLanes> terrain_surface_lanes(const ShaderLookup &p_shaders, std::string_view p_asset) {
	const std::string name = to_lower_ascii(p_asset) + std::string(kTerrainSurfaceShader);
	const SurfaceShader *shader = p_shaders.by_name(name);
	if (shader == nullptr) {
		return core::make_error(name + " is not in the shader database");
	}
	std::optional<TerrainSurfaceLanes> lanes;
	for (const ShaderSolution &s : shader->solutions) {
		if (!s.pixel_program) {
			continue;
		}
		core::Result<dxbc::Program> program = dxbc::parse(*s.pixel_program);
		if (!program) {
			return core::error_at(name, program.error());
		}
		const bool gbuffer = std::any_of(program->outputs.begin(), program->outputs.end(),
				[](const dxbc::SignatureElement &p_e) { return p_e.name == "SV_Target" && p_e.reg == 1; });
		if (!gbuffer) {
			continue;
		}
		const uint8_t output = uint8_t(dxbc::OperandType::Output);
		const size_t end = program->code.size();
		core::Result<TerrainLane> smoothness = trace_surface_lane(*program, end, output, 0, 3);
		core::Result<TerrainLane> specular = trace_surface_lane(*program, end, output, 1, 3);
		TerrainSurfaceLanes found;
		if (smoothness && specular) {
			found.smoothness = *smoothness;
			found.specular = *specular;
		} else {
			found.smoothness.literal = kTerrainUntracedSmoothness;
			found.specular = kTerrainUntracedSpecular;
			found.refusal =
					name + (smoothness ? ": o1.w " + specular.error().message : ": o0.w " + smoothness.error().message);
		}
		const bool same = lanes && lanes->specular == found.specular && lanes->smoothness == found.smoothness &&
				lanes->refusal.empty() == found.refusal.empty();
		if (lanes && !same) {
			return core::make_error(name + "'s G-buffer solutions write o0.w or o1.w differently");
		}
		if (!lanes) {
			lanes = found;
		}
	}
	if (!lanes) {
		return core::make_error(name + " has no G-buffer solution");
	}
	return *lanes;
}

core::Result<TerrainLane> terrain_output_lane(const dxbc::Program &p_program, uint32_t p_output, uint8_t p_lane) {
	return trace_surface_lane(p_program, p_program.code.size(), uint8_t(dxbc::OperandType::Output), p_output, p_lane);
}

std::string terrain_lane_code(const TerrainLane &p_lane, std::string_view p_cover) {
	if (p_lane.literal) {
		return std::to_string(*p_lane.literal);
	}
	const std::pair<uint8_t, uint8_t> source = kTerrainAtlasLanes[p_lane.atlas][p_lane.lane];
	std::string code = "abs(o" + std::to_string(source.first) + "." + "xyzw"[source.second] + ")";
	code = p_cover.empty() ? code : "(" + code + " / " + std::string(p_cover) + ")";
	if (p_lane.scale == 1.0f && p_lane.bias == 0.0f) {
		return code;
	}
	return "(" + code + " * " + std::to_string(p_lane.scale) + " + " + std::to_string(p_lane.bias) + ")";
}

// STAND-IN: a patch takes the combination of exactly its present layers, else the smallest superset, else one for the
// layers from the top opaque layer up, else the one sharing most. Settle: the engine's combination choice read.
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
	if (p_translation.outputs < 3) {
		return core::make_error(
				"terrain combination writes " + std::to_string(p_translation.outputs) + " targets, not 3");
	}
	for (uint32_t input : p_translation.inputs) {
		if (input != 1) {
			return core::make_error("terrain combination reads input v" + std::to_string(input));
		}
	}
	core::Result<std::string> head =
			terrain_shader_head(p_translation, p_material, "terrain", "blend_mix, depth_draw_opaque, cull_back");
	if (!head) {
		return head.error();
	}
	std::vector<uint32_t> mask_slots, colour_slots;
	for (const TerrainTextureBinding &b : p_material.textures) {
		if (b.image == kTerrainMaskAtlas) {
			mask_slots.push_back(b.slot);
		} else if (b.image == kTerrainColourImage) {
			colour_slots.push_back(b.slot);
		}
	}
	core::Result<TerrainDetailCode> detail = terrain_detail_code(mask_slots, colour_slots, p_translation.body);
	if (!detail) {
		return detail.error();
	}
	std::string code = std::move(*head) + light_pass_head();
	code += detail->functions;
	code += "void fragment() {\nvec4 v1 = vec4(UV.x, UV.y, 0.0, 0.0);\n";
	code += terrain_shader_locals(p_translation, p_material);
	code += detail->body;
	// INFERRED: the combination's colour is (o0.y, o0.z, o2.x) and its world normal (o0.x, o1.z, o1.x) x 2 - 1, as the
	// atlas encode packs them. Settle: the terrain surface shader's atlas reads translated.
	code += "vec3 terrain_colour = vec3(o0.y, o0.z, o2.x);\n"
			"vec3 terrain_normal = normalize(vec3(o0.x, o1.z, o1.x) * 2.0 - 1.0);\n" +
			terrain_page_output("terrain_colour", "terrain_normal");
	const std::string specular = terrain_lane_code(p_material.surface.specular, {});
	code += light_pass_gloss(terrain_lane_code(p_material.surface.smoothness, {}),
					terrain_page_camera() + " ? 0.0 : " + specular + " * " + specular) +
			"}\n" + light_pass_code();
	return code;
}

core::Result<TerrainLook> build_terrain_look(const TerrainLookInput &p_input) {
	if (p_input.tree == nullptr || !p_input.tree->mask || p_input.heights == nullptr) {
		return core::make_error("terrain look needs the mask layer and the heights");
	}
	if (p_input.combos.empty()) {
		return core::make_error("terrain look needs its combinations");
	}
	if (!p_input.surface) {
		return core::make_error("terrain look needs its surface shader's smoothness and specular lanes");
	}
	for (const SceneTerrainCombo &c : p_input.combos) {
		if (c.program == nullptr) {
			return core::make_error("terrain combination " + c.shader + " has no pixel program");
		}
	}
	const core::res::TerrainStreamingTree &tree = *p_input.tree;
	const core::res::TerrainMaskTree &mask = *tree.mask;
	const std::array<float, 4> &rect = p_input.region;
	if (!(rect[2] > rect[0]) || !(rect[3] > rect[1])) {
		return core::make_error("terrain look region is empty");
	}
	TerrainLook look;
	look.surface = *p_input.surface;
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
	look.detail.mask = g;
	look.detail.mask_columns = columns;
	look.detail.mask_tiles = tiles;
	look.detail.mask_depth = mask.max_level();
	look.detail.data_mask_texel = look.stats.data_mask_texel;
	look.detail.mask_grid_origin = { mask.bounds[0], mask.bounds[1] };
	look.detail.region = rect;
	if (tree.colour) {
		look.detail.colour = colours.grid;
		look.detail.colour_depth = tree.colour->max_level();
		look.detail.data_colour_texel = look.stats.data_colour_texel;
		look.detail.colour_grid_origin = { tree.colour->bounds[0], tree.colour->bounds[1] };
	}

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

	// STAND-IN: the dynamic mask is one texel of 0. Settle: what the engine writes into it.
	TerrainImage dynamic;
	dynamic.format = TerrainImageFormat::R8;
	dynamic.width = dynamic.height = 1;
	dynamic.data.assign(1, 0);

	for (const TerrainImage *img : { &atlas, &colour, &height, &dynamic }) {
		look.stats.image_bytes += img->data.size();
	}
	look.images = { std::move(atlas), std::move(colour), std::move(height), std::move(dynamic) };
	look.height_uv = height_uv;
	look.height_pixel = height_pixel;

	const std::array<float, 4> colour_uv =
			uv_transform(colours.grid, 0.0f, 0.0f, float(colours.grid.width), float(colours.grid.height));
	for (uint32_t combo_index : used_combos) {
		const SceneTerrainCombo &combo = p_input.combos[combo_index];
		const std::string where = "terrain combination " + combo.shader;
		core::Result<dxbc::Program> program = dxbc::parse(*combo.program);
		if (!program) {
			return core::error_at(where, program.error());
		}
		core::Result<PixelTranslation> translation = translate_pixel_program(*program);
		if (!translation) {
			return core::error_at(where, translation.error());
		}
		look.stats.instructions += translation->instructions;
		TerrainMaterial material;
		material.surface = look.surface;
		material.combo = combo_index;
		const auto terrain_value = [&](const std::string &p_name) -> core::Result<std::array<float, 4>> {
			if (p_name == "external_worldToGlobalColorUvTransform") {
				return colour_uv;
			}
			// WHY: the height image holds metres, so the height scale is 1.
			if (p_name == "external_heightmapWorldSizeY") {
				return std::array<float, 4>{ 1.0f, 0.0f, 0.0f, 0.0f };
			}
			if (!p_name.starts_with(kMaskPrefix) || !p_name.ends_with(kMaskSuffix)) {
				return core::make_error(where + ": unknown constant " + p_name);
			}
			const std::string_view digits = std::string_view(p_name).substr(
					kMaskPrefix.size(), p_name.size() - kMaskPrefix.size() - kMaskSuffix.size());
			unsigned n = 0;
			// INFERRED: worldToMask<n> addresses the combination's n-th layer other than layer 0.
			// Settle: the engine's set-up of those constants read.
			std::vector<uint8_t> masked;
			for (uint8_t l : combo.layers) {
				if (l != 0) {
					masked.push_back(l);
				}
			}
			if (std::from_chars(digits.data(), digits.data() + digits.size(), n).ec != std::errc() ||
					n >= masked.size()) {
				return core::make_error(where + ": " + p_name + " has no masked layer");
			}
			const uint32_t tile = masked[n];
			return uv_transform(
					g, float((tile % columns) * tile_w + 1), float((tile / columns) * tile_h + 1), atlas_w, atlas_h);
		};
		const core::Status bound = bind_terrain_program(
				where, *program, *translation, combo.textures, height_uv, height_pixel, terrain_value, material);
		if (!bound) {
			return bound.error();
		}
		for (const TerrainTextureBinding &b : material.textures) {
			if (b.image == kTerrainMaskAtlas) {
				material.uniforms.emplace_back(
						"detail_mask_coarse", std::array<float, 4>{ g.origin[0], g.origin[1], g.texel, 0.0f });
				material.uniforms.emplace_back("detail_mask_coarse_atlas",
						std::array<float, 4>{ atlas_w, atlas_h, float(tile_w), float(tile_h) });
			} else if (b.image == kTerrainColourImage) {
				material.uniforms.emplace_back("detail_colour_coarse",
						std::array<float, 4>{
								colours.grid.origin[0], colours.grid.origin[1], colours.grid.texel, 0.0f });
				material.uniforms.emplace_back("detail_colour_coarse_size",
						std::array<float, 4>{ float(colours.grid.width), float(colours.grid.height), 0.0f, 0.0f });
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
