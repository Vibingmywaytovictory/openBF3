#pragma once

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
	std::string error;

	bool compressed = false;
	uint8_t version = 0;
	uint64_t body_bytes = 0;
	SwfRect stage;
	float frame_rate = 0.0f;
	uint16_t frame_count = 0;
	std::vector<SwfTagCount> tags;
	uint32_t sprites = 0;
	uint32_t labels = 0;
	uint32_t placements = 0;
	uint32_t edit_texts = 0;
	uint32_t shapes = 0;
	uint32_t scaling_grids = 0;
	uint32_t exports = 0;
	uint32_t imported_symbols = 0;
	uint32_t missing_imports = 0;
	std::vector<std::string> missing_import_symbols;
	std::vector<SwfExternalImage> external_images;
	uint32_t atlas_textures_missing = 0;
	std::vector<SwfSubImage> sub_images;
	uint32_t sub_images_outside = 0;
	std::vector<SwfFontInfo> fonts;
	uint64_t action_bytes = 0;
	uint64_t abc_bytes = 0;
	uint32_t unknown_tags = 0;
	size_t budget_used = 0;
	size_t retained_bytes = 0;
	uint32_t labels_past_last_frame = 0;
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
	uint32_t multi_sha1_names = 0;
	uint32_t labels_past_last_frame = 0;
	size_t max_budget_used = 0;
	size_t max_retained_bytes = 0;
	uint64_t sum_retained_bytes = 0;

	std::array<uint32_t, 5> font_names_by_rule{};
	std::vector<std::string> family_names;

	std::vector<std::pair<std::string, FontResolution>> font_names;

	uint32_t library_font_names = 0;
	uint32_t library_font_names_exact = 0;
	std::vector<std::string> library_font_names_not_exact;
	size_t max_atlas_budget_used = 0;
};

struct SwfCensus {
	std::vector<SwfMovieCensus> movies;
	SwfCensusTotals totals;
	double seconds = 0.0;
};

struct SwfCensusOptions {
	unsigned threads = 0;
	bool check_atlas_textures = true;
	bool build_atlas_maps = true;

	const FontMappingCollection *font_mapping = nullptr;

	const std::vector<FontMappingCollection> *library_mappings = nullptr;
};

Result<SwfCensus> run_swf_census(const SwfLibrary &p_movies, const SwfCensusOptions &p_options = {});

std::string format_census_tsv(const SwfCensus &p_census);
std::string format_census_totals(const SwfCensus &p_census);

}
