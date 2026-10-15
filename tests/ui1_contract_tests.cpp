// UI groundwork 1 interface contract (docs/design/ui-swf-fonts.md §12): every ui1 header compiles on its own set of
// includes, the plain types keep the properties later waves rely on, and the inline helpers behave as the design
// says. No data, no Godot; nothing here calls a function the implementation has yet to write.
#include <doctest/doctest.h>

#include "bf3/core/cache/cache_key.h"
#include "bf3/core/ui/font_ttf.h"
#include "bf3/core/ui/swf_atlas.h"
#include "bf3/core/ui/swf_census.h"
#include "bf3/core/ui/swf_font.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"
#include "bf3/core/ui/ui_fonts.h"

#include <cmath>
#include <string>
#include <type_traits>

namespace ui = bf3::core::ui;

// ---- compile-time contract ---------------------------------------------------------------------------------

static_assert(ui::kSwfMaxBodyBytes == 64u << 20);
static_assert(ui::swf_default_budget_units(0) == size_t(1) << 20);
static_assert(ui::swf_default_budget_units(1000) == (size_t(1) << 20) + 2000);
// 1024 em-square x 20 twips; the TrueType writer divides by exactly 10.
static_assert(ui::kSwfFontUnitsPerEm == 20480);
static_assert(ui::kSwfFontUnitsPerEm == int32_t(ui::kTrueTypeUnitsPerEm) * 10);
static_assert(ui::swf_tag::kGfxDefineSubImage == 1008 && ui::swf_tag::kGfxDefineExternalImage2 == 1009);
static_assert(ui::swf_tag::kDefineFont3 == 75 && ui::swf_tag::kPlaceObject3 == 70 && ui::swf_tag::kDefineScalingGrid == 78);
static_assert(std::is_trivially_copyable_v<ui::SwfMatrix>);
static_assert(std::is_trivially_copyable_v<ui::SwfSubImage>);
static_assert(std::is_trivially_copyable_v<ui::GlyphPoint>);
static_assert(std::is_trivially_copyable_v<ui::KerningPair>);
static_assert(std::is_move_constructible_v<ui::SwfLibrary>);
static_assert(!std::is_copy_constructible_v<ui::SwfLibrary>);

// swf_concat is constexpr: the identity is neutral and translations add up in the parent's space.
constexpr ui::SwfMatrix kTranslate{ 1, 0, 0, 1, 10, 20 };
constexpr ui::SwfMatrix kScale2{ 2, 0, 0, 2, 0, 0 };
static_assert(ui::swf_concat(ui::SwfMatrix{}, kTranslate) == kTranslate);
static_assert(ui::swf_concat(kTranslate, ui::SwfMatrix{}) == kTranslate);
static_assert(ui::swf_concat(kScale2, kTranslate).tx == 20 && ui::swf_concat(kScale2, kTranslate).ty == 40);
static_assert(ui::swf_concat(kTranslate, kScale2).tx == 10 && ui::swf_concat(kTranslate, kScale2).a == 2);

TEST_SUITE("ui1-contract") {
	TEST_CASE("sub-image rectangles are half-open") {
		ui::SwfSubImage s;
		s.x1 = 1091;
		s.y1 = 58;
		s.x2 = 1115;
		s.y2 = 79;
		CHECK(s.width() == 24);
		CHECK(s.height() == 21);
	}

	TEST_CASE("edit-text flags are the DefineEditText bit layout (first byte high)") {
		ui::SwfEditText t;
		t.flags = ui::SwfEditText::kHasFont | ui::SwfEditText::kUseOutlines | ui::SwfEditText::kHasText;
		CHECK(t.has(ui::SwfEditText::kHasFont));
		CHECK(t.has(ui::SwfEditText::kUseOutlines));
		CHECK_FALSE(t.has(ui::SwfEditText::kHtml));
	}

	TEST_CASE("rotation composes like the SWF matrix (x' = a x + c y + tx)") {
		const float s = std::sqrt(0.5f);
		const ui::SwfMatrix rot45{ s, s, -s, s, 0, 0 };
		const ui::SwfMatrix m = ui::swf_concat(rot45, rot45); // 90 degrees
		CHECK(std::fabs(m.a) < 1e-6f);
		CHECK(std::fabs(m.b - 1.0f) < 1e-6f);
		CHECK(std::fabs(m.c + 1.0f) < 1e-6f);
		CHECK(std::fabs(m.d) < 1e-6f);
	}

	TEST_CASE("the font cache kind is a valid cache kind and the key moves with the converter version") {
		CHECK(bf3::core::is_valid_cache_kind(ui::kUiFontCacheKind));
		bf3::core::CacheKeyInputs in;
		in.kind = std::string(ui::kUiFontCacheKind);
		in.sources.resize(1);
		in.converter_version = ui::kUiFontConverterVersion;
		in.engine = "4.8.dev7.c971f93e7";
		in.format_version = ui::kTrueTypeWriterVersion;
		in.params = "font=Purista EA Medium;upem=2048";
		const auto a = bf3::core::make_cache_key(in);
		in.converter_version += 1;
		const auto b = bf3::core::make_cache_key(in);
		REQUIRE(a.has_value());
		REQUIRE(b.has_value());
		CHECK(a->digest.hex() != b->digest.hex());
	}

	TEST_CASE("plain defaults the readers rely on") {
		CHECK(ui::SwfColorTransform{}.mul[0] == 256);
		CHECK(ui::SwfFontInfo{}.kind == ui::SwfFontKind::Outlines);
		CHECK(ui::TrueTypeOptions{}.units_per_em == 2048);
		CHECK(ui::UiFontLibraryOptions{}.text_database == "localization/us_loc");
		CHECK(ui::SwfLayoutOptions{}.max_depth == ui::kSwfLayoutMaxDepth);
	}
}
