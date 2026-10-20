#pragma once

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

inline constexpr size_t kSwfLayoutBudgetUnits = size_t(1) << 22;

enum class SwfLayoutKind : uint8_t {
	Sprite,
	Shape,
	EditText,
	StaticText,
	Import,
	Other,
};

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
	std::string font;
	float size = 0.0f;
	SwfColor color;
	uint8_t align = 0;
	SwfRect bounds;
	std::string initial_text;
	bool html = false;
};

struct SwfLayoutNode {
	std::string instance;
	SwfLayoutKind kind = SwfLayoutKind::Other;
	std::string movie;
	std::string symbol;
	uint16_t depth = 0;
	SwfMatrix local;
	SwfMatrix stage;
	SwfLayoutText text;
	bool has_scaling_grid = false;
	SwfRect scaling_grid;
	std::vector<SwfLayoutNode> children;
};

struct SwfLayoutOptions {
	uint32_t max_depth = kSwfLayoutMaxDepth;
	bool follow_imports = true;
};

Result<SwfLayoutNode> read_layout(const SwfLibrary &p_movies, std::string_view p_movie, const SwfLayoutOptions &p_options, WorkBudget &r_budget);

struct HudPlacement {
	std::string movie;
	SwfRect stage;
	std::string screen_symbol;
	std::string instance;
	std::string widget;
	std::string widget_movie;
	uint16_t depth = 0;
	SwfMatrix matrix;
	HudAnchor anchor = HudAnchor::Centre;
	float anchor_dx = 0.0f;
	float anchor_dy = 0.0f;
};

std::span<const std::string_view> hud_screen_movies();

Result<std::vector<HudPlacement>> read_hud_placements(const SwfLibrary &p_movies, std::string_view p_movie, WorkBudget &r_budget);

}
