#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr uint32_t kSwfAtlasMaxNesting = 16;
inline constexpr uint32_t kSwfAtlasMaxPiecesPerRegion = 4096;
inline constexpr uint32_t kSwfAtlasMaxPieces = 1u << 20;
inline constexpr size_t kSwfAtlasPieceUnits = 4;
inline constexpr size_t kSwfAtlasBudgetUnits = size_t(1) << 24;

struct SwfAtlasImage {
	uint16_t image_id = 0;
	std::string file_name;
	std::string texture_res;
	uint16_t width = 0;
	uint16_t height = 0;
};

struct SwfAtlasPiece {
	uint16_t sub_image = 0;
	uint16_t image_id = 0;
	uint16_t x1 = 0;
	uint16_t y1 = 0;
	uint16_t x2 = 0;
	uint16_t y2 = 0;

	SwfMatrix placement;
};

struct SwfAtlasRegion {

	std::string name;
	uint16_t sprite = 0;
	std::string label;
	uint32_t frame = 0;
	std::vector<SwfAtlasPiece> pieces;
};

struct SwfAtlasStats {
	uint32_t labels = 0;
	uint32_t labels_with_art = 0;
	uint32_t labels_one_piece = 0;
	uint32_t labels_several_pieces = 0;
	uint32_t labels_empty = 0;
	uint32_t sub_images = 0;
	uint32_t sub_images_from_labels = 0;
	uint32_t sub_images_from_symbols = 0;
	uint32_t sub_images_from_frames = 0;
	uint32_t rects_outside_atlas = 0;
	uint32_t pieces_without_image = 0;
};

struct SwfAtlasMap {
	std::string movie;
	std::vector<SwfAtlasImage> images;
	std::vector<SwfAtlasRegion> labels;
	std::vector<SwfAtlasRegion> symbols;
	std::vector<SwfAtlasRegion> frames;
	std::vector<SwfAtlasRegion> bare;
	SwfAtlasStats stats;

	const SwfAtlasRegion *find(std::string_view p_name) const;
};

Result<SwfAtlasMap> build_atlas_map(const SwfMovie &p_movie, std::string_view p_movie_name, WorkBudget &r_budget);

}
