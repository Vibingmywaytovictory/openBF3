#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/font_ttf.h"
#include "bf3/core/ui/swf_font.h"
#include "bf3/core/ui/swf_library.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Cache;
class EbxResolver;
}

namespace bf3::core::ui {

inline constexpr uint32_t kUiFontConverterVersion = 1;
inline constexpr std::string_view kUiFontCacheKind = "ui-font";

inline constexpr size_t kUiFontConvertBudgetUnits = size_t(1) << 25;

struct FontMapping {
	std::string font_long_name;
	std::vector<std::string> scaleform_names;
};

struct FontMappingCollection {
	std::string partition;
	std::string name;
	std::string text_database;
	std::vector<FontMapping> fonts;
	std::string font_library;
};

Result<FontMappingCollection> read_font_mapping(const EbxResolver &p_ebx, std::string_view p_partition);

Result<std::vector<std::string>> font_mapping_partitions(const EbxResolver &p_ebx);

Result<std::vector<FontMappingCollection>> read_all_font_mappings(const EbxResolver &p_ebx);

Result<FontMappingCollection> font_mapping_for_text_database(const EbxResolver &p_ebx, std::string_view p_text_database);

enum class FontResolution : uint8_t {
	Exact,
	CaseFolded,
	Family,

	Device,
	Unresolved,
};
const char *font_resolution_name(FontResolution p_how);

struct ResolvedFont {
	FontResolution how = FontResolution::Unresolved;
	std::string font_long_name;
	std::string via;
};

ResolvedFont resolve_font_name(const FontMappingCollection &p_collection, std::string_view p_used_name);

constexpr std::string_view font_name_count_key(std::string_view p_name) {
	return (!p_name.empty() && p_name.back() == '*') ? p_name.substr(0, p_name.size() - 1) : p_name;
}

struct ConvertedFont {
	std::string font_long_name;
	std::vector<std::string> scaleform_names;
	uint16_t source_glyphs = 0;
	uint16_t distinct_codes = 0;
	std::vector<uint16_t> codes;
	std::vector<uint8_t> ttf;
	TrueTypeStats stats;
	CachedItem cache;
};

struct UiFontLibraryOptions {
	std::string text_database = "localization/us_loc";
	Cache *cache = nullptr;
};

class UiFontLibrary {
public:
	static Result<UiFontLibrary> load(const SwfLibrary &p_movies, const EbxResolver &p_ui_ebx, const UiFontLibraryOptions &p_options);

	const FontMappingCollection &mapping() const { return mapping_; }
	const std::vector<ConvertedFont> &fonts() const { return fonts_; }

	const ConvertedFont *find(std::string_view p_name) const;
	double load_seconds() const { return load_seconds_; }

private:
	FontMappingCollection mapping_;
	std::vector<ConvertedFont> fonts_;
	double load_seconds_ = 0.0;
};

Result<ConvertedFont> convert_library_font(const SwfLibrary &p_movies, std::string_view p_font_library, std::string_view p_font_long_name,
		Cache *p_cache, WorkBudget &r_budget);

}
