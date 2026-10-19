#pragma once
// M2b ui: what the six TDM HUD screens share (hud_tdm.h; docs/design/m2b.md §4; as built: docs/design/m2b-ui.md §3):
// the context (the model drawn, the resolved fonts, strings, icons and placements, the draw records and the lookup
// counters), the screens' base Control and the drawing helpers. Main thread only.
//
// Every screen is a full-window Control under Bf3TdmHud's root that ignores the mouse (the spawn screen's buttons
// take clicks). Bf3TdmHud::set_model calls update_screen() on each (visibility, the records' visibility edges, the
// spawn screen's buttons), and a visible screen redraws from the model in draw_screen() (a C++ virtual called from
// NOTIFICATION_DRAW, as the M2a HUD widgets do: Godot 4.8 dev7 does not dispatch a second-level extension class's
// registered _draw). Placement: BF3's 1280 x 720 stage mapped by hud_stage_to_window around each element's anchor,
// scale min(w / 1280, h / 720) (ui-swf-fonts.md §8.3).
//
// Lookups: fonts through Bf3UiAssets::get_font_file (a name that resolves only to a system font is missing), strings
// through Bf3Localisation's database (hud_tdm_text.h), the kit and dead icons through Bf3UiAssets::get_region, the
// weapon icons through the M2a HUD's loaded _small icons (Bf3Hud::icon; the node is found in the tree). A lookup is
// tried once its source finished loading; each distinct failure counts once in TdmHudRecords and is drawn with a
// fallback (Godot's default font, the SID itself, no icon), never an error.

#include "hud_tdm.h"
#include "hud_tdm_text.h"

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/swf_layout.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace bf3::godot_side {

class Bf3Hud;

// Colours [inferred: BF3's HUD white, its friendly blue and enemy orange, the own player's yellow].
inline const godot::Color kTdmTextColour(0.92f, 0.95f, 0.95f, 1.0f);
inline const godot::Color kTdmDimColour(0.62f, 0.68f, 0.70f, 1.0f);
inline const godot::Color kTdmFriendlyColour(0.42f, 0.78f, 1.0f, 1.0f);
inline const godot::Color kTdmEnemyColour(1.0f, 0.47f, 0.28f, 1.0f);
inline const godot::Color kTdmLocalColour(1.0f, 0.86f, 0.35f, 1.0f);
inline const godot::Color kTdmPanelColour(0.02f, 0.05f, 0.07f, 0.74f);
inline const godot::Color kTdmLineColour(0.55f, 0.75f, 0.80f, 0.45f);
inline const godot::Color kTdmShadowColour(0.0f, 0.0f, 0.0f, 0.6f);
inline const godot::Color kTdmDimBackColour(0.0f, 0.0f, 0.0f, 0.42f); // behind the spawn screen and the round end

enum class TdmFont : uint8_t { Number = 0, Text = 1, Title = 2 };

class TdmHudContext {
public:
	core::ui::HudModel model{};
	TdmHudRecords records{};
	Bf3Localisation *localisation = nullptr;
	Bf3UiAssets *assets = nullptr;

	// Resolves what became ready since the last frame (fonts, the static strings, the icons, the placements, the M2a
	// HUD for the weapon icons). p_tree_node: any node in the tree (to find the M2a HUD).
	void resolve(godot::Node *p_tree_node);
	bool sources_done() const { return text_done_ && assets_done_; }

	godot::Ref<godot::Font> font(TdmFont p_font) const;
	const std::string &text(TdmText p_key) const; // the localised text, or the SID while it is missing
	const std::string &localised(uint32_t p_sid_hash); // a weapon name; "" for 0 or missing (counted once)
	godot::Ref<godot::Texture2D> kit_icon(int p_kit) const;
	godot::Ref<godot::Texture2D> dead_icon() const { return dead_icon_; }
	godot::Ref<godot::Texture2D> weapon_icon(uint32_t p_item_id); // null for 0 or missing (counted once)
	// The resolved stage position and anchor of a placement (its retail value while the tree is not read).
	TdmPlacement placement(const TdmPlacement &p_fallback) const;
	static double now_s(); // monotonic seconds (the kill feed's fade clock)

private:
	void resolve_assets();
	void resolve_text();
	bool text_done_ = false;
	bool assets_done_ = false;
	std::array<godot::Ref<godot::Font>, 3> fonts_;
	std::array<std::string, kTdmTextCount> texts_;
	std::array<godot::Ref<godot::Texture2D>, core::ui::kHudKitCount> kit_icons_;
	godot::Ref<godot::Texture2D> dead_icon_;
	std::array<TdmPlacement, kTdmPlacements.size()> placements_ = kTdmPlacements;
	std::unordered_map<uint32_t, std::string> names_;
	std::unordered_set<uint32_t> missing_items_;
	uint64_t m2a_hud_ = 0; // the M2a HUD's instance id (0: not found yet)
	uint32_t hud_search_countdown_ = 0;
};

// Drawing helpers (window pixels).
float tdm_scale(const godot::Control &p_control); // hud_scale of the control's size
godot::Vector2 tdm_at(const godot::Control &p_control, core::ui::HudAnchor p_anchor, float p_stage_x, float p_stage_y);
int32_t tdm_px(float p_stage_px, float p_scale); // a font size: stage pixels times the scale, at least 1
float tdm_text_width(const godot::Ref<godot::Font> &p_font, const std::string &p_text, int32_t p_size);
// Draws one line with a shadow; p_align 0 left of p_at, 1 right (p_at is the end), 2 centred. Returns the width.
float tdm_draw_text(godot::CanvasItem &p_item, const godot::Ref<godot::Font> &p_font, godot::Vector2 p_at, const std::string &p_text, int32_t p_size,
		const godot::Color &p_colour, int p_align = 0);
void tdm_draw_panel(godot::CanvasItem &p_item, const godot::Rect2 &p_rect, const godot::Color &p_fill, const godot::Color &p_line, float p_line_width);
std::string tdm_player_name(const core::ui::HudModel &p_model, uint8_t p_slot); // "" for kHudNoPlayer, "#<slot>" when empty
godot::Color tdm_player_colour(const core::ui::HudModel &p_model, uint8_t p_slot); // local, friendly or enemy

// The screens' base: a full-window Control that ignores the mouse.
class Bf3TdmScreen : public godot::Control {
	GDCLASS(Bf3TdmScreen, godot::Control)

public:
	void set_context(TdmHudContext *p_context) { ctx_ = p_context; }
	// Once, after the screen is in the tree (child Controls are made here, not in _ready).
	virtual void build_screen() {}
	// Every frame after set_model: visibility, the records' visibility, child layout; redraws when visible.
	virtual void update_screen() {}
	virtual void draw_screen() {}

protected:
	static void _bind_methods() {}
	void _notification(int p_what);
	// Shows or hides the screen and redraws it while shown.
	void show_if(bool p_visible);
	TdmHudContext *ctx_ = nullptr;
};

} // namespace bf3::godot_side
