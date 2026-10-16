#pragma once
// The SwfMovie census (docs/design/ui-swf-fonts.md §5, gate G1): every SwfMovie of the install parsed, with per-movie
// tag counts, labels, sub-image rectangles, fonts, external images and AS2 byte counts, plus the cross-movie checks
// (imports resolve to a movie and an export, atlas textures exist with the declared size, font names resolve).
// Godot-free; bf3dump `swf census` and ctest core.ui.swf print / assert it.
//
// Research-probe numbers for the 434 movies (scratch probe, 2026-10-15; the C++ census must reproduce them):
// 371 CFX v8, 62 GFX v8, 1 GFX v7; 0 parse errors. Tag totals: PlaceObject2 27,832, ShowFrame 23,917, End 3,217,
// DefineSprite 2,783, DoAction 2,456, FrameLabel 2,441, ExportAssets 1,855, DoInitAction 1,538, DefineEditText
// 1,488, ImportAssets2 1,483, DefineShape 982, PlaceObject3 980, CSMTextSettings 916, GFX_DefineSubImage 820,
// RemoveObject2 629, GFX_ExporterInfo 434, FileAttributes 434, Metadata 401, SetBackgroundColor 401,
// GFX_DefineExternalImage2 135, DefineShape3 117, DefineFont3 86, DefineShape2 53, DefineScalingGrid 29, DefineText
// 12, DefineShape4 7, ImportAssets 1; DoABC, DefineFontAlignZones, DefineFontName, DefineFont2 and the other GFx
// tags 0. AS2 bytecode 1,638,265 bytes. 135 external images (one per atlas movie), 1,268 imported symbols of which
// one names a missing export (deckofcards imports IconsShared_Check, which iconsshared does not export).

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"
#include "bf3/core/ui/ui_fonts.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::core::ui {

struct SwfMovieCensus {
	std::string name;
	std::string superbundle;
	uint32_t holders = 0;
	uint32_t distinct_sha1s = 0;
	uint64_t payload_bytes = 0;
	std::string error; // empty when the movie parsed, its atlas textures exist with their size, its sub-images lie
	                   // inside them and its atlas map builds (a missing import is counted, not an error)
	// Parsed facts (zero / empty on a parse error).
	bool compressed = false;
	uint8_t version = 0;
	uint64_t body_bytes = 0;
	SwfRect stage;
	float frame_rate = 0.0f;
	uint16_t frame_count = 0;
	std::vector<SwfTagCount> tags;
	uint32_t sprites = 0;
	uint32_t labels = 0;
	uint32_t placements = 0; // PlaceObject/2/3 events, root + sprites
	uint32_t edit_texts = 0;
	uint32_t shapes = 0;
	uint32_t scaling_grids = 0;
	uint32_t exports = 0;
	uint32_t imported_symbols = 0;
	uint32_t missing_imports = 0; // symbols whose movie or export does not exist (listed, not an error: retail has one)
	std::vector<std::string> missing_import_symbols; // "<url>:<symbol>"
	std::vector<SwfExternalImage> external_images;
	uint32_t atlas_textures_missing = 0; // external images without their DxTexture (or of another size)
	std::vector<SwfSubImage> sub_images;
	uint32_t sub_images_outside = 0;
	std::vector<SwfFontInfo> fonts;
	uint64_t action_bytes = 0;
	uint64_t abc_bytes = 0;
	uint32_t unknown_tags = 0;
	size_t budget_used = 0;
	size_t retained_bytes = 0; // SwfMovie::retained_bytes
	uint32_t labels_past_last_frame = 0; // labels whose frame == their timeline's ShowFrame count (0 in retail; G1)
	double parse_ms = 0.0;
};

struct SwfCensusTotals {
	uint32_t movies = 0;
	uint32_t parsed = 0;
	uint32_t errors = 0;
	uint32_t cfx = 0;
	uint32_t gfx = 0;
	std::vector<SwfTagCount> tags;
	uint64_t action_bytes = 0;
	uint64_t abc_bytes = 0;
	uint64_t payload_bytes = 0;
	uint64_t body_bytes = 0;
	uint32_t labels = 0;
	uint32_t sub_images = 0;
	uint32_t external_images = 0;
	uint32_t imported_symbols = 0;
	uint32_t missing_imports = 0;
	uint32_t atlas_textures_missing = 0;
	uint32_t fonts_outlines = 0;
	uint32_t fonts_no_glyphs = 0;
	uint32_t fonts_codes_only = 0;
	uint32_t multi_sha1_names = 0; // names whose holders disagree: G1 fails unless 0 (listed by format_census_totals)
	uint32_t labels_past_last_frame = 0;
	size_t max_budget_used = 0;
	size_t max_retained_bytes = 0;
	uint64_t sum_retained_bytes = 0;
	// Font names over all movies (gfxfontlib imports, glyphless DefineFont3 names, edit-text font classes) by
	// FontResolution (index = the enum value), counted as distinct names after dropping one trailing '*'
	// (font_name_count_key, ui_fonts.h): expected Exact 4, CaseFolded 2, Family 4, Device 5, Unresolved 0 (G6).
	// family_names is printed on its own line ("G6 family-inferred: ...").
	std::array<uint32_t, 5> font_names_by_rule{};
	std::vector<std::string> family_names;
	// Every distinct counted name with its FontResolution, sorted by name (empty without a font mapping).
	std::vector<std::pair<std::string, FontResolution>> font_names;
	// The other font libraries' own edit-text fonts (ja's DFP_sougeitai_W5, the EULA and store fonts, ...) are not
	// counted against font_mapping; with library_mappings each is resolved against the collection that maps that
	// library (distinct (library, name) pairs; all Exact on retail).
	uint32_t library_font_names = 0;
	uint32_t library_font_names_exact = 0;
	std::vector<std::string> library_font_names_not_exact; // "<library>: <name>"
	size_t max_atlas_budget_used = 0; // build_atlas_map over the atlas movies (kSwfAtlasBudgetUnits each)
};

struct SwfCensus {
	std::vector<SwfMovieCensus> movies; // sorted by name
	SwfCensusTotals totals;
	double seconds = 0.0; // parse + checks, wall (the library's open time is separate)
};

struct SwfCensusOptions {
	unsigned threads = 0; // 0 = all cores
	bool check_atlas_textures = true; // reads each atlas DxTexture header (not its chunk)
	bool build_atlas_maps = true; // build_atlas_map of every movie with an external image
	// The collection the font names are resolved with (G6: the en one); null: font names are not counted. The names
	// are those of every movie except the font libraries other than font_mapping's own (see library_font_names).
	const FontMappingCollection *font_mapping = nullptr;
	// Every UIFontMappingCollection (for library_font_names); may be null.
	const std::vector<FontMappingCollection> *library_mappings = nullptr;
};

Result<SwfCensus> run_swf_census(const SwfLibrary &p_movies, const SwfCensusOptions &p_options = {});

// One TSV row per movie (the bf3dump `swf census --tsv` format; header line first) and a totals block.
std::string format_census_tsv(const SwfCensus &p_census);
std::string format_census_totals(const SwfCensus &p_census);

} // namespace bf3::core::ui
