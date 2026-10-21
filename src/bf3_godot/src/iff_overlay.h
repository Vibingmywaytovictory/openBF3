#pragma once

#include "iff_frame.h"
#include "minimap_control.h"

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/nametags.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Camera3D;
class CanvasLayer;
}

namespace bf3::godot_side {

class Bf3LevelSession;

inline constexpr uint32_t kMaxLosRaysPerFrame = 4;

struct NametagDraw {
	uint8_t slot = 0;
	core::ui::TagRelation relation = core::ui::TagRelation::None;
	bool icon = false;
	bool name = false;
	float scale = 1.0f;
	float alpha = 1.0f;
	float distance_m = 0.0f;
	uint8_t kit = kIffNoKit;
	godot::Vector2 anchor;
	godot::String text;
};

class Bf3Nametags : public godot::Control {
	GDCLASS(Bf3Nametags, godot::Control)

public:
	void _draw() override;
	void setup(godot::Ref<godot::ImageTexture> p_atlas, const core::ui::HudIconAtlas *p_icons);
	void set_style(const core::ui::NametagStyle &p_style, const std::array<std::string, 4> &p_kit_icons);
	void set_font(const godot::Ref<godot::Font> &p_font) { font_ = p_font; }
	void update(std::vector<NametagDraw> p_draws, float p_canvas_scale);
	const std::vector<NametagDraw> &draws() const { return draws_; }

protected:
	static void _bind_methods();

private:
	void draw_frame(const core::ui::HudIcon *p_icon, core::ui::HudIconState p_state, godot::Vector2 p_centre, float p_size, float p_alpha,
			const godot::Color &p_modulate = godot::Color(1, 1, 1, 1));

	godot::Ref<godot::ImageTexture> atlas_;
	const core::ui::HudIconAtlas *icons_ = nullptr;
	core::ui::NametagStyle style_;
	std::array<std::string, 4> kit_icons_;
	godot::Ref<godot::Font> font_;
	std::vector<NametagDraw> draws_;
	float canvas_scale_ = 1.0f;
};

struct IffOverlayStats {
	uint64_t frames = 0;
	uint64_t friendly_icons = 0;
	uint64_t friendly_names = 0;
	uint64_t enemy_names = 0;
	uint64_t los_rays = 0;
	uint64_t los_blocked = 0;
	uint64_t minimap_friendly = 0;
	uint64_t minimap_enemy = 0;
	uint64_t minimap_hidden_enemies = 0;
	uint64_t all_others_as_enemies = 0;
	uint64_t unknown_team = 0;
	uint64_t frame_ns = 0;
	uint64_t frame_max_ns = 0;
};

class IffOverlay {
public:
	IffOverlay(Bf3LevelSession *p_session, godot::CanvasLayer *p_layer, godot::Ref<godot::ImageTexture> p_atlas, const core::ui::HudIconAtlas *p_icons);
	~IffOverlay();
	IffOverlay(const IffOverlay &) = delete;
	IffOverlay &operator=(const IffOverlay &) = delete;

	void frame(const godot::Camera3D *p_camera, bool p_first_person, std::vector<MinimapIconDraw> &r_icons);
	const IffOverlayStats &stats() const { return stats_; }

private:
	struct Loaded;
	void run_loader();
	void step_shots(const godot::Camera3D *p_camera, double p_now_s);
	void log_summary();

	Bf3LevelSession *session_ = nullptr;
	Bf3Nametags *tags_ = nullptr;
	godot::Ref<godot::ImageTexture> atlas_;
	const core::ui::HudIconAtlas *icons_ = nullptr;
	std::thread loader_;
	std::atomic<bool> published_{ false };
	std::atomic<bool> stop_{ false };
	std::mutex mutex_;
	std::shared_ptr<const Loaded> loaded_;
	bool applied_ = false;
	core::ui::NametagStyle style_;
	core::ui::SpottingRules spotting_;
	IffReveal reveal_;
	IffOverlayStats stats_;
	bool font_set_ = false;
	double summary_at_s_ = -1.0;

	struct Shots;
	std::unique_ptr<Shots> shots_;

	double live_seconds_ = 0.0;
	double live_start_s_ = -1.0;
	uint32_t reveal_peak_ = 0;
	std::string live_dir_;
};

}
