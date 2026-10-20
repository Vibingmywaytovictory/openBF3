#pragma once

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

inline constexpr float kHudNameRight = 1090.0f;
inline constexpr float kHudNameBaseline = 676.0f;
inline constexpr float kHudNameSize = 16.0f;
inline constexpr float kHudIconTop = 638.0f;
inline constexpr float kHudIconWidth = 64.0f;
inline constexpr float kHudIconHeight = 16.0f;

inline constexpr float kCrosshairTickLength = 9.0f;
inline constexpr float kCrosshairTickWidth = 2.0f;
inline constexpr float kCrosshairDotRadius = 1.5f;

inline constexpr double kHudDefaultVfovDeg = 55.0;

inline const godot::Color kHudTextColor(0.92f, 0.95f, 0.95f, 1.0f);
inline const godot::Color kHudCriticalColor(1.0f, 0.36f, 0.18f, 1.0f);
inline const godot::Color kHudShadowColor(0.0f, 0.0f, 0.0f, 0.55f);
inline const godot::Color kCrosshairColor(0.85f, 1.0f, 0.85f, 0.95f);

class Bf3Hud;

struct HudDrawRecord {
	uint64_t serial = 0;
	uint64_t draws = 0;
	bool visible = false;
	std::string text_a;
	std::string text_b;
	int32_t size_a = 0;
	int32_t size_b = 0;
	godot::Vector2 at_a;
	godot::Vector2 at_b;
	std::string font;
	double gap_px = 0;
	godot::Vector2 centre;
	bool art = false;
	godot::Vector2 icon_at;
	godot::Vector2 icon_size;
	uint32_t icon_item = 0;
	bool critical = false;
};

class Bf3HudWidget : public godot::Control {
	GDCLASS(Bf3HudWidget, godot::Control)

public:
	void set_hud(Bf3Hud *p_hud) { hud_ = p_hud; }
	const HudDrawRecord &record() const { return record_; }

	virtual void draw_widget() {}

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

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

	static godot::PackedStringArray required_atlas_movies();

	void set_model(const bf3::core::ui::HudModel &p_model);
	const bf3::core::ui::HudModel &model() const { return model_; }

	void set_vfov_deg(double p_deg);
	double vfov_deg() const;

	bool is_loaded() const { return finished_; }
	bool has_layout() const { return layout_ok_; }
	const bf3::host::HudScreenLayout &layout() const { return layout_; }

	const bf3::core::ui::ItemIconIndex *item_index() const;
	godot::Ref<godot::Texture2D> icon(uint32_t p_item_id) const;
	size_t icon_count() const { return icons_.size(); }
	size_t icons_failed() const { return icons_failed_; }
	std::string load_error() const { return error_; }

	godot::Ref<godot::Font> number_font() const { return number_font_; }
	godot::Ref<godot::Font> text_font() const { return text_font_; }
	const std::string &number_font_name() const { return number_font_name_; }
	const std::string &text_font_name() const { return text_font_name_; }
	const std::string &localised(uint32_t p_sid_hash);
	struct HitPiece {
		godot::Ref<godot::AtlasTexture> texture;
		godot::Transform2D transform;
	};
	const std::vector<HitPiece> &hit_pieces(float p_level) const;

	float number_ascent_per_px();
	float text_ascent_per_px();

	Bf3HudAmmo *ammo_widget() const { return ammo_; }
	Bf3HudCrosshair *crosshair_widget() const { return crosshair_; }
	Bf3HudHitIndicator *hit_widget() const { return hit_; }
	Bf3HudWeaponName *name_widget() const { return name_; }
	Bf3HudHealth *health_widget() const { return health_; }

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
	std::unique_ptr<Loaded> loaded_;
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
	std::vector<std::vector<HitPiece>> hit_levels_;
	std::unordered_map<uint32_t, std::string> names_;
};

}
