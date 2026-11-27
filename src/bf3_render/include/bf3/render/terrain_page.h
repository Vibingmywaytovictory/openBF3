#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace bf3::render {

inline constexpr uint32_t kTerrainPageColourLayer = 1u << 18;
inline constexpr uint32_t kTerrainPageNormalLayer = 1u << 19;
// STAND-IN: terrain pages are 512 texels a side, drawn top down from the terrain's own look.
// Settle: the engine's virtual texture pages read.
inline constexpr uint32_t kTerrainPageSide = 512;
inline constexpr std::array<std::string_view, 2> kTerrainPageAtlases{ "texture_virtualTextureAtlas0",
	"texture_virtualTextureAtlas1" };
inline constexpr std::string_view kTerrainPageUv = "external_worldToTerrainTextureUv";
inline constexpr std::string_view kTerrainPageUvUniform = "terrain_page_uv";

int32_t terrain_page_atlas(std::string_view p_binding);

std::string terrain_page_function(int32_t p_atlas);

std::string terrain_page_sampling();

std::string terrain_page_output(std::string_view p_colour, std::string_view p_normal);

std::string terrain_page_camera();

std::string terrain_page_hidden();

std::array<float, 4> terrain_page_uv(float p_origin_x, float p_origin_z, float p_side);

}
