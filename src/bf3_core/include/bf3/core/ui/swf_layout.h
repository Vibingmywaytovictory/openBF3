#pragma once
// HUD layout facts from the movies' timelines (docs/design/ui-swf-fonts.md §8): where BF3 places each HUD widget
// on its 1280 x 720 stage, at what scale, and what the widget's own text fields and art are, read at run time from
// the install (never committed). The later HUD waves lay out their own Controls from this tree. Godot-free.
//
// Shape of a retail HUD screen (measured 2026-10-15): the root timeline places one instance ("instance1") of the
// exported screen sprite (HudMPScreen, HudTDMScreen, ...) at identity; that sprite's frame 0 places the widgets,
// each an ImportAssets2 symbol of another movie (Ammo.swf:Ammo, Minimap.swf:Minimap, ...) under an instance name
// (Ammo_01, Minimap_01). Examples: hudmpscreen Ammo_01 (1194, 669), Health_01 (1193, 605), VehicleHealth_01
// (1195, 555), Hitindicator_01 (640, 360), LatencyIndicator_01 (1274, 3); hudtdmscreen Minimap_01 (-3, 527),
// Compass_01 (85, 696), TicketCounter_01 (-4, 530), SquadList_00 (179, 619), HudBackgroundWidget_01 (-3, 486.9)
// scaled (1.288, 3.689). Inside a widget the same pattern repeats (ammo: the Ammo sprite at the stage centre,
// edit texts in $HUD_medium 21 / 43 px and $Menu_medium 20 / 21 px, white). weaponcommonscreen places nothing at
// frame 0: the crosshairs are attached by AS2, so they stay procedural (plan §3.8).
//
// The tree follows imports into the exporting movie (SwfLibrary) up to kSwfLayoutMaxDepth levels; every node and
// every followed import charges the budget, and a movie is parsed once per call.

#include "bf3/core/result.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_movie.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr uint32_t kSwfLayoutMaxDepth = 8;
inline constexpr uint32_t kSwfLayoutMaxNodes = 1u << 16;

enum class SwfLayoutKind : uint8_t {
	Sprite,
	Shape,
	EditText,
	StaticText,
	Import, // an imported symbol whose movie could not be followed (depth cap, missing export, font library)
	Other,
};

// [inferred] Which stage edge a widget hugs: by its origin's position in thirds of the stage (x < 1/3 left,
// > 2/3 right; y alike). The HUD wave keeps the offset from that anchor when the window is not 16:9.
enum class HudAnchor : uint8_t {
	TopLeft,
	Top,
	TopRight,
	Left,
	Centre,
	Right,
	BottomLeft,
	Bottom,
	BottomRight,
};
const char *hud_anchor_name(HudAnchor p_anchor);
HudAnchor hud_anchor_for(float p_x, float p_y, const SwfRect &p_stage);

struct SwfLayoutText {
	std::string font; // the font's name as the movie names it ("$HUD_medium", "Purista EA Medium"); resolve with ui_fonts.h
	float size = 0.0f; // pixels
	SwfColor color;
	uint8_t align = 0;
	SwfRect bounds;
	std::string initial_text; // "000", "ID_H_RELOAD_HINT"
	bool html = false;
};

struct SwfLayoutNode {
	std::string instance; // instance name, "" when unnamed
	SwfLayoutKind kind = SwfLayoutKind::Other;
	std::string movie; // the movie that defines the character ("ui/assets/ammo")
	std::string symbol; // export name or "#<id>"
	uint16_t depth = 0;
	SwfMatrix local; // in the parent's space
	SwfMatrix stage; // composed down from the screen's stage
	SwfLayoutText text; // EditText only
	bool has_scaling_grid = false;
	SwfRect scaling_grid;
	std::vector<SwfLayoutNode> children; // display order
};

struct SwfLayoutOptions {
	uint32_t max_depth = kSwfLayoutMaxDepth;
	bool follow_imports = true;
};

// The layout tree of a movie's root timeline at frame 0 (each child sprite at its frame 0).
Result<SwfLayoutNode> read_layout(const SwfLibrary &p_movies, std::string_view p_movie, const SwfLayoutOptions &p_options, WorkBudget &r_budget);

// One row of the HUD positions table: a widget a HUD screen places, in stage pixels.
struct HudPlacement {
	std::string movie; // "ui/assets/hudmpscreen"
	SwfRect stage;
	std::string screen_symbol; // "HudMPScreen"
	std::string instance; // "Ammo_01"
	std::string widget; // "Ammo" (the import's symbol)
	std::string widget_movie; // "ui/assets/ammo"
	uint16_t depth = 0;
	SwfMatrix matrix; // in stage pixels
	HudAnchor anchor = HudAnchor::Centre;
	float anchor_dx = 0.0f; // origin minus the anchor point, pixels
	float anchor_dy = 0.0f;
};

// The in-game MP HUD screens the table covers: hudmpscreen, hudscreen, hudconquestscreen, hudtdmscreen,
// hudsdmscreen, hudrushscreen, hudctfscreen, hudgmscreen, hudmatchpreroundscreen, hudpreroundscreen, killscreen,
// spawnscreenpc, spectatorscreen, weaponcommonscreen (names without "ui/assets/"). Measured: 86 widget
// placements over the 14 (14, 8, 9, 7, 8, 9, 10, 7, 0, 1, 4, 8, 1, 0), every screen on a 1280 x 720 stage.
std::span<const std::string_view> hud_screen_movies();

Result<std::vector<HudPlacement>> read_hud_placements(const SwfLibrary &p_movies, std::string_view p_movie, WorkBudget &r_budget);

} // namespace bf3::core::ui
