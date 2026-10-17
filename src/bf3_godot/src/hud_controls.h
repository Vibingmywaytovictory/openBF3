#pragma once
// The minimal HUD (docs/design/m2a.md §11.3, §11.4, §15 row hud; as built: docs/design/m2a-hud.md): Bf3Hud, a
// full-window Control that draws ui2's HudModel and nothing else (it never reads the sim; bf3/host/hud_feed.h fills
// the model). Its five widgets are child Controls, one per HUD element:
//
//   Bf3HudAmmo          magazine / reserve in $HUD_medium at the hudmpscreen Ammo_01 text slots (clip 43 px and
//                       reserve 21 px, right-aligned, the "/" between them); the low-ammo colour while
//                       active_weapon_critical_ammo; hidden while !active_weapon_show_ammo (the knife)
//   Bf3HudCrosshair     four ticks around the window centre at gap = crosshair_gap_px(h, dispersion, vfov); a dot
//                       for the knife; hidden while !show_crosshair (ADS)
//   Bf3HudHitIndicator  the hitindicator movie's #4/level1 art (four mirrored 15 x 15 pieces at +-18 stage px) at
//                       Hitindicator_01 while show_hitindicator; procedural ticks when the atlas is missing
//   Bf3HudWeaponName    the localised active_weapon_name ($Menu_medium) and the active_weapon_data item's _small
//                       icon (ItemIconIndex -> load_item_icon, DXT5 64 x 16), left of the magazine digits
//   Bf3HudHealth        round(health) and "%" at the Health_01 text slots while the soldier is alive
//
// Placement: every stage point maps to the window by hud_stage_to_window around its widget's anchor, scale
// min(w / 1280, h / 720) (ui-swf-fonts.md §8.3); font sizes are the slot sizes times that scale, rounded to whole
// pixels (Godot's font sizes are integers). The weapon name and icon have no slot in hudmpscreen that this track
// reads (BF3 draws them inside the ammo widget's own frames); their stage placement below is this track's
// [inferred].
//
// Loading: Bf3Hud waits for Bf3Localisation and Bf3UiAssets (the scene's; Bf3UiAssets must list
// required_atlas_movies() before it enters the tree for the hit indicator art, else the procedural fallback is
// drawn), then on its own std::thread (no Godot API there) reads the hudmpscreen layout (read_layout +
// make_hud_screen_layout), the Win32/UI item index and the _small icon of every weapon and kit item description.
// Back on the main thread it makes one ImageTexture per icon and emits "hud_loaded". Until then, and for anything
// that failed (logged once), it draws from fallback placement and without icons.
//
// Draw records: each widget keeps what its last _draw drew (the model serial, visibility, texts, sizes, window
// positions); the m2a_hud check (hud_check.h) compares them with the model every frame.

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/host/hud_feed.h"

#include <godot_cpp/classes/atlas_texture.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bf3::godot_side {

// Weapon name and icon, stage pixels (Ammo_01's BottomRight anchor) [inferred]: both right-aligned at
// kHudNameRight, left of the clip field (it starts at x 1092); the icon above the name.
inline constexpr float kHudNameRight = 1090.0f;
inline constexpr float kHudNameBaseline = 676.0f;
inline constexpr float kHudNameSize = 16.0f; // $Menu_medium, the ammo widget's label size
inline constexpr float kHudIconTop = 638.0f; // the _small icons are 64 x 16
inline constexpr float kHudIconWidth = 64.0f;
inline constexpr float kHudIconHeight = 16.0f;
// Crosshair ticks, stage pixels scaled like the rest [inferred: BF3's rifle crosshair is four short ticks].
inline constexpr float kCrosshairTickLength = 9.0f;
inline constexpr float kCrosshairTickWidth = 2.0f;
inline constexpr float kCrosshairDotRadius = 1.5f; // the knife / no-spread kinds
// The camera's vertical field of view when none is set and the viewport has no Camera3D (Godot's default 75 is not
// BF3's; 55 is BF3's default infantry vertical FOV at 16:9 [inferred from the 70 degree horizontal setting]).
inline constexpr double kHudDefaultVfovDeg = 55.0;
// Colours [inferred: BF3's HUD white, its low-ammo orange-red].
inline const godot::Color kHudTextColor(0.92f, 0.95f, 0.95f, 1.0f);
inline const godot::Color kHudCriticalColor(1.0f, 0.36f, 0.18f, 1.0f);
inline const godot::Color kHudShadowColor(0.0f, 0.0f, 0.0f, 0.55f);
inline const godot::Color kCrosshairColor(0.85f, 1.0f, 0.85f, 0.95f);

class Bf3Hud;

// What one widget's last _draw drew (window pixels).
struct HudDrawRecord {
	uint64_t serial = 0; // HudModel::serial of the model drawn
	uint64_t draws = 0;
	bool visible = false;
	std::string text_a; // ammo: magazine; health: value; name: the localised name
	std::string text_b; // ammo: reserve
	int32_t size_a = 0; // font sizes (px)
	int32_t size_b = 0;
	godot::Vector2 at_a; // the text's alignment point on its baseline (right end for right-aligned text)
	godot::Vector2 at_b;
	std::string font; // the font name drawn with
	double gap_px = 0; // crosshair: the gap drawn; hit indicator: the art's scale
	godot::Vector2 centre; // crosshair / hit indicator: the centre drawn around
	bool art = false; // hit indicator: atlas pieces drawn (not the fallback); name: the icon drawn
	godot::Vector2 icon_at; // name: the icon's top-left and size
	godot::Vector2 icon_size;
	uint32_t icon_item = 0;
	bool critical = false;
};

// Base of the five widgets: a full-rect child of Bf3Hud, ignoring the mouse.
class Bf3HudWidget : public godot::Control {
	GDCLASS(Bf3HudWidget, godot::Control)

public:
	void set_hud(Bf3Hud *p_hud) { hud_ = p_hud; }
	const HudDrawRecord &record() const { return record_; }

	// What the widget draws on NOTIFICATION_DRAW (a C++ virtual: the widgets are second-level extension classes, and
	// Godot 4.8 dev7 did not dispatch their registered _draw override in this build; the notification always arrives).
	virtual void draw_widget() {}

protected:
	static void _bind_methods() {}
	void _notification(int p_what);
	// Begins a record for the model being drawn.
	void begin_record();
	Bf3Hud *hud_ = nullptr;
	HudDrawRecord record_;
};

class Bf3HudAmmo : public Bf3HudWidget {
	GDCLASS(Bf3HudAmmo, Bf3HudWidget)

public:
	void draw_widget() override;

protected:
	static void _bind_methods() {}
};

class Bf3HudCrosshair : public Bf3HudWidget {
	GDCLASS(Bf3HudCrosshair, Bf3HudWidget)

public:
	void draw_widget() override;

protected:
	static void _bind_methods() {}
};

class Bf3HudHitIndicator : public Bf3HudWidget {
	GDCLASS(Bf3HudHitIndicator, Bf3HudWidget)

public:
	void draw_widget() override;

protected:
	static void _bind_methods() {}
};

class Bf3HudWeaponName : public Bf3HudWidget {
	GDCLASS(Bf3HudWeaponName, Bf3HudWidget)

public:
	void draw_widget() override;

protected:
	static void _bind_methods() {}
};

class Bf3HudHealth : public Bf3HudWidget {
	GDCLASS(Bf3HudHealth, Bf3HudWidget)

public:
	void draw_widget() override;

protected:
	static void _bind_methods() {}
};

class Bf3Hud : public godot::Control {
	GDCLASS(Bf3Hud, godot::Control)

public:
	Bf3Hud();
	~Bf3Hud() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

	// The movies Bf3UiAssets must load atlases of for the HUD (its defaults plus the hit indicator's).
	static godot::PackedStringArray required_atlas_movies();

	// The model drawn from the next frame on (main thread); every widget redraws.
	void set_model(const bf3::core::ui::HudModel &p_model);
	const bf3::core::ui::HudModel &model() const { return model_; }
	// The vertical field of view the crosshair gap uses (degrees); <= 0: the viewport's Camera3D, else
	// kHudDefaultVfovDeg.
	void set_vfov_deg(double p_deg);
	double vfov_deg() const;

	bool is_loaded() const { return finished_; }
	bool has_layout() const { return layout_ok_; }
	const bf3::host::HudScreenLayout &layout() const { return layout_; }
	// The item index the loader read (null before "hud_loaded" or when it failed): the game's HudStatic takes the
	// M67's name from it (apply_item_names).
	const bf3::core::ui::ItemIconIndex *item_index() const;
	godot::Ref<godot::Texture2D> icon(uint32_t p_item_id) const;
	size_t icon_count() const { return icons_.size(); }
	size_t icons_failed() const { return icons_failed_; }
	std::string load_error() const { return error_; }

	// The resolved drawing inputs (main thread): fonts, the localised name of a SID hash ("" when missing), the hit
	// indicator art of a level (empty: the fallback).
	godot::Ref<godot::Font> number_font() const { return number_font_; }
	godot::Ref<godot::Font> text_font() const { return text_font_; }
	const std::string &number_font_name() const { return number_font_name_; }
	const std::string &text_font_name() const { return text_font_name_; }
	const std::string &localised(uint32_t p_sid_hash);
	struct HitPiece {
		godot::Ref<godot::AtlasTexture> texture;
		godot::Transform2D transform; // sub-image pixels -> the symbol's stage pixels
	};
	const std::vector<HitPiece> &hit_pieces(float p_level) const;
	// Ascent per pixel of font size of the number font (hud_text_anchor), measured once.
	float number_ascent_per_px();
	float text_ascent_per_px();

	Bf3HudAmmo *ammo_widget() const { return ammo_; }
	Bf3HudCrosshair *crosshair_widget() const { return crosshair_; }
	Bf3HudHitIndicator *hit_widget() const { return hit_; }
	Bf3HudWeaponName *name_widget() const { return name_; }
	Bf3HudHealth *health_widget() const { return health_; }

	// The slots drawn from: the loaded layout's, else fallback placement (the retail values, ui-swf-fonts.md §8).
	const bf3::host::HudTextSlot &slot_clip() const;
	const bf3::host::HudTextSlot &slot_reserve() const;
	const bf3::host::HudTextSlot &slot_slash() const;
	const bf3::host::HudTextSlot &slot_health() const;
	const bf3::host::HudTextSlot &slot_percent() const;
	bf3::core::ui::HudAnchor ammo_anchor() const;
	bf3::core::ui::HudAnchor health_anchor() const;
	bf3::host::HudPoint hit_centre_stage() const;

protected:
	static void _bind_methods();

private:
	struct Loaded;
	void join_loader();
	void start_loader();
	void finish_load();
	void resolve_assets();
	void redraw_all();

	bf3::core::ui::HudModel model_{};
	double vfov_deg_ = 0;
	Bf3HudAmmo *ammo_ = nullptr;
	Bf3HudCrosshair *crosshair_ = nullptr;
	Bf3HudHitIndicator *hit_ = nullptr;
	Bf3HudWeaponName *name_ = nullptr;
	Bf3HudHealth *health_ = nullptr;

	std::thread loader_;
	std::atomic<bool> loader_done_{ false };
	std::unique_ptr<Loaded> loaded_; // written by the loader before loader_done_, read on the main thread after
	bool finished_ = false;
	bool assets_resolved_ = false;
	bool layout_ok_ = false;
	bf3::host::HudScreenLayout layout_;
	bf3::host::HudScreenLayout fallback_;
	std::unordered_map<uint32_t, godot::Ref<godot::Texture2D>> icons_;
	size_t icons_failed_ = 0;
	std::string error_;
	godot::Ref<godot::Font> number_font_;
	godot::Ref<godot::Font> text_font_;
	std::string number_font_name_;
	std::string text_font_name_;
	float number_ascent_ = -1;
	float text_ascent_ = -1;
	std::vector<std::vector<HitPiece>> hit_levels_; // index = level - 1
	std::unordered_map<uint32_t, std::string> names_;
};

} // namespace bf3::godot_side
