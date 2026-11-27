#include "bf3/render/terrain_page.h"

namespace bf3::render {

int32_t terrain_page_atlas(std::string_view p_binding) {
	for (size_t i = 0; i < kTerrainPageAtlases.size(); ++i) {
		if (p_binding == kTerrainPageAtlases[i]) {
			return int32_t(i);
		}
	}
	return -1;
}

std::string terrain_page_function(int32_t p_atlas) {
	return "terrain_atlas" + std::to_string(p_atlas);
}

// INFERRED: from the atlas encode (kTerrainAtlasLanes), atlas 0 reads (colour, normal x) and atlas 1 (o1.y, normal y,
// o2.y, normal z), whose x and z read 0 here. Settle: the engine's virtual texture pages read.
std::string terrain_page_sampling() {
	const std::string uv(kTerrainPageUvUniform);
	return "uniform sampler2D terrain_page_colour : filter_linear, repeat_disable;\n"
		   "uniform sampler2D terrain_page_normal : filter_linear, repeat_disable;\n"
		   "uniform sampler2D terrain_page_far_colour : filter_linear, repeat_disable;\n"
		   "uniform sampler2D terrain_page_far_normal : filter_linear, repeat_disable;\n"
		   "uniform vec4 " +
			uv +
			" = vec4(0.0);\n"
			"uniform vec4 terrain_page_far = vec4(0.0);\n"
			"vec4 terrain_page(sampler2D p_near, sampler2D p_far, vec2 p_uv) {\n"
			"\tif (p_uv.x > 0.0 && p_uv.y > 0.0 && p_uv.x < 1.0 && p_uv.y < 1.0) {\n"
			"\t\treturn texture(p_near, p_uv);\n"
			"\t}\n"
			"\tvec2 world = (p_uv - " +
			uv + ".zw) / max(" + uv +
			".xy, vec2(1e-9));\n"
			"\treturn texture(p_far, world * terrain_page_far.xy + terrain_page_far.zw);\n"
			"}\n"
			"vec4 terrain_atlas0(vec2 p_uv) {\n"
			"\tvec3 colour = terrain_page(terrain_page_colour, terrain_page_far_colour, p_uv).rgb;\n"
			"\treturn vec4(colour, terrain_page(terrain_page_normal, terrain_page_far_normal, p_uv).r);\n"
			"}\n"
			"vec4 terrain_atlas1(vec2 p_uv) {\n"
			"\tvec3 n = terrain_page(terrain_page_normal, terrain_page_far_normal, p_uv).rgb;\n"
			"\treturn vec4(0.0, n.g, 0.0, n.b);\n"
			"}\n";
}

// WHY: the page camera takes the colour or normal through EMISSION with albedo 0, so the page holds them unlit.
std::string terrain_page_output(std::string_view p_colour, std::string_view p_normal) {
	const std::string colour(p_colour), normal(p_normal);
	return "if (" + terrain_page_camera() +
			") {\n"
			"\tvec3 page_normal = normalize((MODEL_MATRIX * vec4(" +
			normal +
			", 0.0)).xyz) * 0.5 + 0.5;\n"
			"\tEMISSION = (CAMERA_VISIBLE_LAYERS & " +
			std::to_string(kTerrainPageNormalLayer) + "u) != 0u ? page_normal : " + colour +
			";\n"
			"\tALBEDO = vec3(0.0);\n"
			"\tSPECULAR = 0.0;\n"
			"\tMETALLIC = 0.0;\n"
			"} else {\n"
			"\tALBEDO = " +
			colour + " * " + colour +
			";\n"
			"\tNORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(" +
			normal +
			", 0.0))).xyz);\n"
			"}\n"
			"ROUGHNESS = 1.0;\n";
}

std::string terrain_page_camera() {
	return "(CAMERA_VISIBLE_LAYERS & 1u) == 0u";
}

std::string terrain_page_hidden() {
	return "\tif (" + terrain_page_camera() + ") {\n\t\tdiscard;\n\t}\n";
}
std::array<float, 4> terrain_page_uv(float p_origin_x, float p_origin_z, float p_side) {
	const float scale = p_side > 0.0f ? 1.0f / p_side : 0.0f;
	return { scale, scale, -p_origin_x * scale, -p_origin_z * scale };
}

}
