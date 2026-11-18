#include "bf3/render/terrain_program.h"

#include <algorithm>

namespace bf3::render {

namespace {

constexpr std::string_view kConstants = "externalConstants";
constexpr std::string_view kViewConstants = "viewConstants";

}

core::Status bind_terrain_program(const std::string &p_where, const dxbc::Program &p_program,
		const PixelTranslation &p_translation, std::span<const SceneTerrainTexture> p_textures,
		const std::array<float, 4> &p_height_uv, const std::array<float, 4> &p_height_pixel,
		const TerrainConstantValue &p_value, TerrainMaterial &r_material) {
	for (uint32_t slot : p_translation.textures) {
		TerrainTextureBinding b;
		b.slot = slot;
		const dxbc::Binding *bound = p_program.binding(dxbc::BindingType::Texture, slot);
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
			const auto data = std::find_if(
					p_textures.begin(), p_textures.end(), [&](const SceneTerrainTexture &t) { return t.slot == slot; });
			if (data == p_textures.end() || data->texture < 0) {
				return core::make_error(p_where + ": texture t" + std::to_string(slot) + " (" + name +
						") is not in the shader database");
			}
			b.texture = data->texture;
			b.srgb = data->srgb;
		}
		r_material.textures.push_back(b);
	}
	const dxbc::Binding *constants = p_program.binding(dxbc::BindingType::ConstantBuffer, kConstants);
	const dxbc::Binding *view = p_program.binding(dxbc::BindingType::ConstantBuffer, kViewConstants);
	const dxbc::Variable *time = p_program.variable(kViewConstants, "time");
	for (const auto &[slot, size] : p_translation.constant_buffers) {
		if (view != nullptr && slot == view->bind_point && size == 1 && time != nullptr && time->offset == 0) {
			r_material.view_slot = slot;
			continue;
		}
		if (constants == nullptr || slot != constants->bind_point) {
			return core::make_error(
					p_where + ": reads constant buffer cb" + std::to_string(slot) + " beyond the view time");
		}
		r_material.constant_slot = slot;
		r_material.constants.assign(size, std::array<float, 4>{});
	}
	if (constants == nullptr) {
		return core::Status::success();
	}
	for (const dxbc::ConstantBuffer &cb : p_program.constant_buffers) {
		if (cb.name != kConstants) {
			continue;
		}
		for (const dxbc::Variable &v : cb.variables) {
			std::array<float, 4> value{};
			if (v.name == "external_clipToWorldSpaceXzTransform" || v.name == "external_dynamicMaskUvScale") {
				value = { 1.0f, 1.0f, 0.0f, 0.0f };
			} else if (v.name == "external_worldToHeightfieldUvTransform") {
				value = p_height_uv;
			} else if (v.name == "external_heightfieldPixelSizeAndAspect") {
				value = p_height_pixel;
			} else if (p_value) {
				core::Result<std::array<float, 4>> given = p_value(v.name);
				if (!given) {
					return given.error();
				}
				value = *given;
			} else {
				return core::make_error(p_where + ": unknown constant " + v.name);
			}
			const uint32_t reg = v.offset / 16;
			const uint32_t first = (v.offset % 16) / 4;
			for (uint32_t k = 0; k < v.size / 4 && first + k < 4 && reg < r_material.constants.size(); ++k) {
				r_material.constants[reg][first + k] = value[k];
			}
		}
	}
	return core::Status::success();
}

core::Result<std::string> terrain_shader_head(const PixelTranslation &p_translation, const TerrainMaterial &p_material,
		std::string_view p_what, std::string_view p_render_mode) {
	std::string code = "shader_type spatial;\nrender_mode " + std::string(p_render_mode) + ";\n";
	for (const auto &[slot, size] : p_translation.constant_buffers) {
		if (slot != p_material.view_slot) {
			code += "uniform vec4 cb" + std::to_string(slot) + "[" + std::to_string(size) + "];\n";
		}
	}
	for (uint32_t slot : p_translation.textures) {
		const auto found = std::find_if(p_material.textures.begin(), p_material.textures.end(),
				[&](const TerrainTextureBinding &b) { return b.slot == slot; });
		if (found == p_material.textures.end()) {
			return core::make_error(std::string(p_what) + " texture t" + std::to_string(slot) + " has no binding");
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
	return code;
}

std::string terrain_shader_locals(const PixelTranslation &p_translation, const TerrainMaterial &p_material) {
	std::string code;
	if (p_material.view_slot) {
		code += "vec4 cb" + std::to_string(*p_material.view_slot) + "[1] = vec4[1](vec4(TIME, 0.0, 0.0, 0.0));\n";
	}
	for (uint32_t o = 0; o < p_translation.outputs; ++o) {
		code += "vec4 o" + std::to_string(o) + " = vec4(0.0);\n";
	}
	return code;
}

}
