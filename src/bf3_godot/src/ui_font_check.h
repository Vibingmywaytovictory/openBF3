#pragma once
// The ui1 Godot check (docs/design/ui-swf-fonts.md §1 gates G4/G5, §11): res://scenes/ui_fonts.tscn, a Control
// that shows BF3's fonts and atlases through Bf3UiAssets and checks them.
//
//   headless  -- --ui-check                 PASS/FAIL table, last line "UI1 CHECK: PASS (n/n)", exit 0 / 1
//   windowed  -- --ui-check --shot-dir=<d>  the same, plus screenshots <d>/ui1_fonts.png, ui1_coverage.png,
//                                           ui1_icons.png (written to b\shots\ui1 by ctest; never committed)
//
// Rows: per font (Purista EA Medium / Semibold, Venice-Medium / SemiBold) fonts.set_data (FontFile accepted, font
// name read back), fonts.glyphs (the TrueType maxp count = source glyphs + 1; get_supported_chars() has exactly
// the distinct codes; has_char for each), fonts.contours (TextServer.font_get_glyph_contours for every glyph:
// the same contour count and orientation as the TrueType writer's, i.e. no inverted contour), fonts.metrics
// (ascent / descent / advances of "MÉTRO" and "0123456789" at 64 px against the source layout, within 1 px),
// fonts.kerning (Purista "AV" narrower than "A" + "V" by the kern pair, within 1 px), fonts.metro (U+00C9 shapes
// to a real glyph, not .notdef), fonts.mapping (every $-name of the census resolves), fonts.coverage (reported),
// atlas.iconsingame (162 regions inside 4096 x 128, the label counts of swf_atlas.h), atlas.regions (AtlasTexture
// rectangles of the design's spot checks; the uploaded ImageTexture sizes), hud.table (86 placements on the 14
// screens), install.unchanged. The row list in docs/design/ui-swf-fonts.md §11.1 is authoritative.
//
// The screens drawn for the shots: sample strings in each font at 16 / 32 / 64 px on a dark background
// ("OPERATION MÉTRO", the alphabet, "31 / 155", the Venice digits), the coverage table (every code of each font in
// a grid with its hex value), and the iconsingame labels with their regions.

#include <godot_cpp/classes/control.hpp>

#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3UiAssets;

class Bf3UiFontCheck : public godot::Control {
	GDCLASS(Bf3UiFontCheck, godot::Control)

public:
	Bf3UiFontCheck();
	~Bf3UiFontCheck() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _draw() override;

protected:
	static void _bind_methods();

private:
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	void run_checks(Bf3UiAssets &p_assets);
	void finish();

	std::vector<Row> rows_;
	std::string shot_dir_;
	bool check_ = false;
	int page_ = 0;
	int frames_on_page_ = 0;
	bool done_ = false;
};

} // namespace bf3::godot_side
