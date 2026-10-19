#pragma once
// PT1 iff (docs/design/pt1-iff.md): telling friend from foe in the M2b scene. Owned by Bf3MinimapHud (the hud3dicons
// atlas, the session and the active camera are already there; its two `// PT1 iff [...]` lines create this and call
// frame()), so the level session's M2b files are not touched.
//   loader    a std::thread (no Godot API): its own Install from the session's reader and an EbxLibrary over the level
//             superbundle: ui::read_nametag_style (UINametagCompData), ui::read_spotting_rules (the soldier's
//             SpottingTargetComponentData), and each team's kits' UIHudIcon (gameplay/teams/{us,ru} -> kits). Until it
//             publishes (or when it fails) the retail defaults of nametags.h are used.
//   frame     main thread, from Bf3MinimapHud::update_view every frame: iff_soldiers of the session's frame, the own
//             team (PresentationFrame::own_team, else the own player row), IffReveal, then
//             * the minimap's soldiers (iff_minimap): UIHudIcon_Player Friendly / Squad for teammates, Enemy for an
//               enemy only while its own fire reveals it, appended to the minimap's icon list (replacing M1's loop
//               that drew every other soldier as an enemy);
//             * the nametags (ui::decide_nametag) through Bf3Nametags, a full-screen Control under the minimap: the
//               icon UIHudIcon_Player Friendly / Squad over teammates (always, through walls), the kit icon and the
//               name in FriendlyTint / SquadTint within ShowLabelRange or when looked at, an enemy's name in
//               EnemyTint only while looked at with a clear line of sight (at most kMaxLosRaysPerFrame rays a frame
//               against the M2a session's static collision world).
//   shots     --iff-shots=<dir> (windowed, M2b scene): after the own spawn, teleports in front of the nearest
//             teammate and the nearest enemy at 6, 15 and 35 m, looks at each and saves a screenshot per view plus
//             the atlas frames used (iff_atlas_frames.png), logs what each tag decided, then quits (exit 0, 1 when a
//             view failed). Use --preround=120 so nobody moves or fires meanwhile.

#include "iff_frame.h"
#include "minimap_control.h"

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/nametags.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/image_texture.hpp>
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
} // namespace godot

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
	godot::Vector2 anchor; // canvas units: the projected anchor plus PixelOffset
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
	void draw_frame(const core::ui::HudIcon *p_icon, core::ui::HudIconState p_state, godot::Vector2 p_centre, float p_size, float p_alpha);

	godot::Ref<godot::ImageTexture> atlas_;
	const core::ui::HudIconAtlas *icons_ = nullptr;
	core::ui::NametagStyle style_;
	std::array<std::string, 4> kit_icons_;
	godot::Ref<godot::Font> font_;
	std::vector<NametagDraw> draws_;
	float canvas_scale_ = 1.0f;
};

struct Pt1IffStats {
	uint64_t frames = 0;
	uint64_t friendly_icons = 0; // nametag icons drawn (sum over frames)
	uint64_t friendly_names = 0;
	uint64_t enemy_names = 0;
	uint64_t los_rays = 0;
	uint64_t los_blocked = 0;
	uint64_t minimap_friendly = 0; // soldier-frames on the minimap
	uint64_t minimap_enemy = 0;
	uint64_t minimap_hidden_enemies = 0;
	uint64_t m1_rule_enemy_icons = 0; // what the M1 loop would have drawn as enemies (every other soldier): the before
	uint64_t unknown_team = 0; // soldier-frames without a team (no tag)
};

class Pt1Iff {
public:
	Pt1Iff(Bf3LevelSession *p_session, godot::CanvasLayer *p_layer, godot::Ref<godot::ImageTexture> p_atlas, const core::ui::HudIconAtlas *p_icons);
	~Pt1Iff(); // joins the loader
	Pt1Iff(const Pt1Iff &) = delete;
	Pt1Iff &operator=(const Pt1Iff &) = delete;

	// Main thread, every frame (Bf3MinimapHud::update_view): appends the minimap's soldiers to r_icons and draws the
	// nametags as seen from p_camera (null or not first person: nametags hidden).
	void frame(const godot::Camera3D *p_camera, bool p_first_person, std::vector<MinimapIconDraw> &r_icons);
	const Pt1IffStats &stats() const { return stats_; }

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
	std::shared_ptr<const Loaded> loaded_; // under mutex_ until taken
	bool applied_ = false;
	core::ui::NametagStyle style_;
	core::ui::SpottingRules spotting_;
	IffReveal reveal_;
	Pt1IffStats stats_;
	bool font_set_ = false;
	double summary_at_s_ = -1.0;
	// --iff-shots
	struct Shots;
	std::unique_ptr<Shots> shots_;
};

} // namespace bf3::godot_side
