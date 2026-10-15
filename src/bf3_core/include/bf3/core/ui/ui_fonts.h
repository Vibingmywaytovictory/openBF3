#pragma once
// BF3's UI fonts for the Godot HUD (docs/design/ui-swf-fonts.md §7): the UIFontMappingCollection that maps
// Scaleform font names ($MENU_medium, $HUD_bold, ...) to the long names of the DefineFont3 fonts in the language's
// font library movie, and those fonts converted to TrueType through the derived-data cache. Godot-free.
//
// Data (Win32/UI, measured 2026-10-15): 33 UIFontMappingCollection partitions, ui/static/fontcollection_<lang>_
// fontmapcollection (11 languages) and ui/static/ondemandfontcollection/{eula,store}fontcollection_<lang>_
// fontmapcollection (11 each). Fields: Name, TextDatabase (a reference to the language's UITextDatabase:
// localization/us_loc for en; empty in the on-demand ones), Fonts[] = UIFontMapping {FontLongName,
// ScaleformFontName[]}. The en collection: Purista EA Medium -> $MENU_medium, Purista EA Semibold -> $MENU_bold,
// Venice-Medium -> $HUD_medium, Venice-SemiBold -> $HUD_bold. The font library movie is the SwfMovie res of the
// same name with "_fontmapcollection" replaced by "_fontlib" (ui/static/fontcollection_en_fontlib, 115,603 bytes,
// GFX v8, 4 DefineFont3 exported under their long names).
//
// The 434 movies name fonts in three ways: imports from "../Static/gfxfontlib.swf" ($Menu_bold 102, $Menu_medium
// 89, $HUD_medium 20, $FONT_TITLE 3, $HUD 1, $HUD_light 1), glyphless DefineFont3 tags named after a library font
// ("$FONT_TITLE*", "$Menu_light*") and glyphless device fonts (Arial 13, Purista SemiBold 3, Calibri 2, Verdana 1,
// Times New Roman 1). resolve_font_name() handles all of them; see FontResolution.
//
// Caching: one entry per converted font, kind "ui-font"; sources = the font library res's manifest SHA-1; converter
// kUiFontConverterVersion; engine id; format = kTrueTypeWriterVersion; params "font=<long name>;upem=2048". The
// payload is the TrueType file itself (FontFile.set_data takes it unchanged); accept = read_truetype() + the
// family name. Converted fonts never leave the cache root (plan §2.1: a converted EA font is a private copy): no
// function here or in bf3dump writes the TrueType bytes to any other path.

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
} // namespace bf3::core

namespace bf3::core::ui {

inline constexpr uint32_t kUiFontConverterVersion = 1;
inline constexpr std::string_view kUiFontCacheKind = "ui-font";
// One budget for the whole conversion of one font: load_font_outlines + normalise_winding + write_truetype +
// read_truetype (convert_library_font creates it; docs §9.1). A hostile 4,096-contour, 16,384-point glyph needs 67M
// winding units and ends on it in well under a second.
inline constexpr size_t kUiFontConvertBudgetUnits = size_t(1) << 25;

struct FontMapping {
	std::string font_long_name; // "Purista EA Medium"
	std::vector<std::string> scaleform_names; // {"$MENU_medium"}
};

struct FontMappingCollection {
	std::string partition; // "ui/static/fontcollection_en_fontmapcollection"
	std::string name; // UIFontMappingCollection.Name
	std::string text_database; // referenced UITextDatabase partition, "" when none
	std::vector<FontMapping> fonts;
	std::string font_library; // "ui/static/fontcollection_en_fontlib"
};

Result<FontMappingCollection> read_font_mapping(const EbxResolver &p_ebx, std::string_view p_partition);
// Every UIFontMappingCollection partition of p_ebx (Win32/UI), sorted.
Result<std::vector<std::string>> font_mapping_partitions(const EbxResolver &p_ebx);
// Every collection of font_mapping_partitions(), read (the census checks each font library against its own).
Result<std::vector<FontMappingCollection>> read_all_font_mappings(const EbxResolver &p_ebx);
// The collection whose TextDatabase is p_text_database ("localization/us_loc" -> the en collection).
Result<FontMappingCollection> font_mapping_for_text_database(const EbxResolver &p_ebx, std::string_view p_text_database);

enum class FontResolution : uint8_t {
	Exact, // a ScaleformFontName or FontLongName of the collection, byte for byte
	CaseFolded, // the same ignoring ASCII case and one trailing '*' ("$Menu_bold" -> $MENU_bold)
	Family, // [inferred] no entry: $HUD* -> $HUD_medium, $MENU* -> $MENU_medium, $FONT_TITLE -> $MENU_bold
	         // ($HUD, $HUD_light, $Menu_light*, $FONT_TITLE*); listed by the census
	Device, // a name without '$' that no FontLongName matches (Arial, Calibri, ...): the system font
	Unresolved, // a '$' name no rule covers: fails the mapping gate
};
const char *font_resolution_name(FontResolution p_how);

struct ResolvedFont {
	FontResolution how = FontResolution::Unresolved;
	std::string font_long_name; // the library font it uses ("" for Device / Unresolved)
	std::string via; // the collection's Scaleform name the rule picked, e.g. "$MENU_bold"
};

ResolvedFont resolve_font_name(const FontMappingCollection &p_collection, std::string_view p_used_name);

// The key gate G6 counts distinct font names by: the name with one trailing '*' dropped ("$FONT_TITLE*" and
// "$FONT_TITLE" are one name).
constexpr std::string_view font_name_count_key(std::string_view p_name) {
	return (!p_name.empty() && p_name.back() == '*') ? p_name.substr(0, p_name.size() - 1) : p_name;
}

struct ConvertedFont {
	std::string font_long_name;
	std::vector<std::string> scaleform_names;
	uint16_t source_glyphs = 0; // 443 / 33
	uint16_t distinct_codes = 0; // 442 / 32
	std::vector<uint16_t> codes; // distinct, ascending (the coverage table)
	std::vector<uint8_t> ttf; // FontFile.set_data input
	TrueTypeStats stats; // zero on a cache hit except glyphs / cmap_codes / bytes (from read_truetype)
	CachedItem cache;
};

struct UiFontLibraryOptions {
	std::string text_database = "localization/us_loc"; // picks the collection (the language)
	Cache *cache = nullptr; // null: convert every time
};

// The language's mapping, its font library movie and every mapped font converted (cache first). Runs on a loader
// thread; the result is immutable and shared.
class UiFontLibrary {
public:
	static Result<UiFontLibrary> load(const SwfLibrary &p_movies, const EbxResolver &p_ui_ebx, const UiFontLibraryOptions &p_options);

	const FontMappingCollection &mapping() const { return mapping_; }
	const std::vector<ConvertedFont> &fonts() const { return fonts_; }
	// A Scaleform or long name through resolve_font_name; nullptr for Device / Unresolved.
	const ConvertedFont *find(std::string_view p_name) const;
	double load_seconds() const { return load_seconds_; }

private:
	FontMappingCollection mapping_;
	std::vector<ConvertedFont> fonts_;
	double load_seconds_ = 0.0;
};

// One font of a font library movie converted through p_cache (may be null); the unit the library and bf3dump use.
Result<ConvertedFont> convert_library_font(const SwfLibrary &p_movies, std::string_view p_font_library, std::string_view p_font_long_name,
		Cache *p_cache, WorkBudget &r_budget); // r_budget: kUiFontConvertBudgetUnits per font

} // namespace bf3::core::ui
