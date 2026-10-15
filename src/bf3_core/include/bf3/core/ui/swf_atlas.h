#pragma once
// The atlas cutter (docs/design/ui-swf-fonts.md §6): a movie's GFx external images (BF3 DxTextures
// ui/assets/<swf>/<swf>_iN) cut into named regions by its DefineSubImage rectangles, and every frame label of
// every sprite resolved to the sub-images its frame shows. Godot-free; the Godot side turns a region into an
// AtlasTexture over the uploaded atlas (bf3_godot ui_assets.h).
//
// Resolution of a labelled frame: the sprite's display list at that frame (display_lists_at, one pass per
// sprite); each entry's character is a shape (its bitmap fills that name a sub-image become pieces), a sprite
// (its frame 0, recursively, at most kSwfAtlasMaxNesting levels, matrices composed) or something else (an edit
// text, an import: no piece). An exported symbol resolves the same way from its frame 0.
//
// iconsingame (measured 2026-10-15): one external image (iconsingame_i1.tga, 4096 x 128, DXT5), 162 sub-images,
// 427 labels on 6 sprites. 135 labelled frames show art (110 one sub-image, 25 two: an icon over a background),
// 292 frames are empty (mc_iconsIngameSmall and IconsIngame_Menu name 144 and 122 states but draw 32 and 8 of
// them; their art is placed at run time by AS2 or not used; a label's span up to the next label adds nothing).
// 95 of the 162 sub-images are reached from a label, 96 with the exported symbols' frame 0, and all 162 from
// some frame of some sprite (the 42 unlabelled child sprites hold the rest, e.g. states a parent switches with
// gotoAndStop), so `frames` lists every (sprite, frame) that draws one. Every rectangle lies inside the atlas
// (820 of 820 over the 135 atlas movies).
//
// Bounds: the recursion is depth-capped and every visited display entry and every piece charges the budget, so
// a sprite that places itself or a wide DAG of sprites ends on the budget, never on time or memory.

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_movie.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr uint32_t kSwfAtlasMaxNesting = 16;
inline constexpr uint32_t kSwfAtlasMaxPiecesPerRegion = 4096;

struct SwfAtlasImage {
	uint16_t image_id = 0;
	std::string file_name; // "iconsingame_i1.tga"
	std::string texture_res; // "ui/assets/iconsingame/iconsingame_i1"
	uint16_t width = 0;
	uint16_t height = 0;
};

// One sub-image drawn as part of a region.
struct SwfAtlasPiece {
	uint16_t sub_image = 0; // character id
	uint16_t image_id = 0;
	uint16_t x1 = 0; // atlas pixels, x2 / y2 exclusive
	uint16_t y1 = 0;
	uint16_t x2 = 0;
	uint16_t y2 = 0;
	// Sub-image pixel space -> the region symbol's space (pixels): the shape's fill matrix (twips per bitmap pixel
	// scaled to pixels) composed with every placement down from the symbol.
	SwfMatrix placement;
};

struct SwfAtlasRegion {
	// "<symbol>/<label>" for a labelled frame, "<symbol>" for an exported symbol's frame 0, "<symbol>@<frame>" for
	// any frame, "#<id>" for a bare sub-image. <symbol> is the sprite's export name, or "#<sprite id>" when it has
	// none.
	std::string name;
	uint16_t sprite = 0; // 0 for a bare sub-image
	std::string label;
	uint32_t frame = 0;
	std::vector<SwfAtlasPiece> pieces; // display order (lowest depth first); empty = the frame draws no sub-image
};

struct SwfAtlasStats {
	uint32_t labels = 0;
	uint32_t labels_with_art = 0;
	uint32_t labels_one_piece = 0;
	uint32_t labels_several_pieces = 0;
	uint32_t labels_empty = 0;
	uint32_t sub_images = 0;
	uint32_t sub_images_from_labels = 0; // distinct sub-images some labelled frame draws
	uint32_t sub_images_from_symbols = 0; // ... some exported symbol draws (frame 0)
	uint32_t sub_images_from_frames = 0; // ... some frame of some sprite draws (162 of 162 for iconsingame)
	uint32_t rects_outside_atlas = 0; // must be 0
	uint32_t pieces_without_image = 0; // a sub-image naming an image id the movie does not define
};

struct SwfAtlasMap {
	std::string movie; // "ui/assets/iconsingame"
	std::vector<SwfAtlasImage> images;
	std::vector<SwfAtlasRegion> labels; // every FrameLabel of every sprite (and of the root timeline), tag order
	std::vector<SwfAtlasRegion> symbols; // every exported sprite or shape
	std::vector<SwfAtlasRegion> frames; // every (sprite, frame) that draws at least one sub-image, "<symbol>@<frame>"
	std::vector<SwfAtlasRegion> bare; // one region per sub-image, "#<id>"
	SwfAtlasStats stats;

	// Exact name; for labels also "<label>" alone when only one sprite of the movie has it.
	const SwfAtlasRegion *find(std::string_view p_name) const;
};

Result<SwfAtlasMap> build_atlas_map(const SwfMovie &p_movie, std::string_view p_movie_name, WorkBudget &r_budget);

} // namespace bf3::core::ui
