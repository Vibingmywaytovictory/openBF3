#pragma once

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


inline const godot::Color kTdmTextColour(0.92f, 0.95f, 0.95f, 1.0f);
inline const godot::Color kTdmDimColour(0.62f, 0.68f, 0.70f, 1.0f);
inline const godot::Color kTdmFriendlyColour(0.42f, 0.78f, 1.0f, 1.0f);
inline const godot::Color kTdmEnemyColour(1.0f, 0.47f, 0.28f, 1.0f);
inline const godot::Color kTdmLocalColour(1.0f, 0.86f, 0.35f, 1.0f);
inline const godot::Color kTdmPanelColour(0.02f, 0.05f, 0.07f, 0.74f);
inline const godot::Color kTdmLineColour(0.55f, 0.75f, 0.80f, 0.45f);
inline const godot::Color kTdmShadowColour(0.0f, 0.0f, 0.0f, 0.6f);
inline const godot::Color kTdmDimBackColour(0.0f, 0.0f, 0.0f, 0.42f);

enum class TdmFont : uint8_t { Number = 0, Text = 1, Title = 2 };

class TdmHudContext {
public:
	core::ui::HudModel model{};
	TdmHudRecords records{};
	Bf3Localisation *localisation = nullptr;
	Bf3UiAssets *assets = nullptr;

	void resolve(godot::Node *p_tree_node);
	bool sources_done() const { return text_done_ && assets_done_; }

	godot::Ref<godot::Font> font(TdmFont p_font) const;
	const std::string &text(TdmText p_key) const;
	const std::string &localised(uint32_t p_sid_hash);
	godot::Ref<godot::Texture2D> kit_icon(int p_kit) const;
	godot::Ref<godot::Texture2D> dead_icon() const { return dead_icon_; }
	godot::Ref<godot::Texture2D> weapon_icon(uint32_t p_item_id);

	TdmPlacement placement(const TdmPlacement &p_fallback) const;
	static double now_s();

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
	uint64_t weapon_hud_id_ = 0;
	uint32_t hud_search_countdown_ = 0;
};

float tdm_scale(const godot::Control &p_control);
godot::Vector2 tdm_at(const godot::Control &p_control, core::ui::HudAnchor p_anchor, float p_stage_x, float p_stage_y);
int32_t tdm_px(float p_stage_px, float p_scale);
float tdm_text_width(const godot::Ref<godot::Font> &p_font, const std::string &p_text, int32_t p_size);

float tdm_draw_text(godot::CanvasItem &p_item, const godot::Ref<godot::Font> &p_font, godot::Vector2 p_at, const std::string &p_text, int32_t p_size,
		const godot::Color &p_colour, int p_align = 0);
void tdm_draw_panel(godot::CanvasItem &p_item, const godot::Rect2 &p_rect, const godot::Color &p_fill, const godot::Color &p_line, float p_line_width);
std::string tdm_player_name(const core::ui::HudModel &p_model, uint8_t p_slot);
godot::Color tdm_player_colour(const core::ui::HudModel &p_model, uint8_t p_slot);

class Bf3TdmScreen : public godot::Control {
	GDCLASS(Bf3TdmScreen, godot::Control)

public:
	void set_context(TdmHudContext *p_context) { ctx_ = p_context; }

	virtual void build_screen() {}

	virtual void update_screen() {}
	virtual void draw_screen() {}

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

	void show_if(bool p_visible);
	TdmHudContext *ctx_ = nullptr;
};

}
