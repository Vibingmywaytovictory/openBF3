#pragma once
// Bf3MinimapHud: BF3's minimap and the flag / HQ world icons in the running M1 game (docs/design/ui-minimap-icons.md
// §10; plan §3.8, D16). A CanvasLayer added to res://scenes/m1_metro.tscn as its own node beside the Session: it reads
// the session through Bf3LevelSession's public accessors and changes nothing there (the lk and s3 tracks edit
// level_session in parallel).
//
// Lifetime:
//   _ready: finds the session (session_path, default "../Session"); does nothing more when the session is headless or
//     has no scene (--headless, --m1-check): the dummy renderer would keep CPU copies of the images.
//   loader std::thread (no Godot API): opens its own Install from the session's InstallReader, then
//     ui::load_minimap_level (load_level + traverse + read) or a "ui-minimap" cache hit (design §8),
//     ui::load_minimap_images (satellite, detail SDF, combat-area SDFs; L8), the mip chain of the satellite,
//     Win32/UI: read_hud_icon_atlas + load_hud_icon_texture (RGBA8). Publishes once with a release store.
//   main thread, after the publish: Image::create_from_data (FORMAT_L8 with the box-filtered chain, FORMAT_RGBA8), the
//     ShaderMaterial (minimap_control.h), the two Controls; then every frame: the own soldier's presented position
//     (fp_camera presented_eye of the session's PresentationFrame) and the view yaw (InputMapper::view, the local mouse
//     state, so the map turns without network latency) -> MinimapView -> queue_redraw.
//   NOTIFICATION_PREDELETE: joins the loader (it checks a stop flag between steps).
//
// User args (after "--"; read by this node only):
//   --no-minimap                     do not create the minimap
//   --minimap-mode=<mode>            read the objectives and combat areas of another mode of the same level (the M1
//                                    game runs TDM, which has no flags; the shots use ConquestSmall0)
//   --minimap-size=<px>              side of the square minimap (default 256 at 1080 lines, scaled by height/1080)
//   --minimap-north-up               the large-map orientation (no rotation; also the M key in game)
//   --minimap-check                  the in-game gates G9-G11 (minimap_check.h): PASS/FAIL table, quits 0 or 1
//   --minimap-shots=<dir>            after Done: screenshots of the HUD (rotating and north-up) at the spawn and at
//                                    the objectives, written to <dir> (b\shots\ui2\, never the repo)
// Keys: M toggles north-up / rotating (BF3 PC opens the large map with M [inferred]); the minimap draws only while the
// first-person camera is current (F6 free camera hides it).

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/minimap.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3MinimapControl;
class Bf3WorldIcons;
class MinimapCheck;

// What the loader publishes (immutable after the publish).
struct MinimapAssets {
	bf3::core::ui::MinimapLevel level;
	bf3::core::ui::MinimapImages images;
	std::vector<bf3::core::ui::L8Image> satellite_mips; // the box-filtered chain, mip 0 first
	bf3::core::ui::HudIconAtlas atlas;
	bf3::core::ui::Rgba8Image atlas_image;
	std::string cache_result; // hit / converted / uncached (design §8)
	double seconds = 0.0; // loader wall time
	std::string error; // non-empty: nothing to draw (logged once)
};

class Bf3MinimapHud : public godot::CanvasLayer {
	GDCLASS(Bf3MinimapHud, godot::CanvasLayer)

public:
	Bf3MinimapHud();
	~Bf3MinimapHud() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _unhandled_input(const godot::Ref<godot::InputEvent> &p_event) override;

	void set_session_path(const godot::NodePath &p_path);
	godot::NodePath get_session_path() const;

	// ---- for the check (main thread) ------------------------------------------------------------------------------
	bool ready_to_draw() const { return drawn_; }
	std::shared_ptr<const MinimapAssets> assets() const;
	const bf3::core::ui::MinimapView &view() const { return view_; }
	Bf3MinimapControl *control() const { return control_; }
	Bf3WorldIcons *world_icons() const { return icons_; }
	Bf3LevelSession *session() const { return session_; }
	void set_north_up(bool p_on);

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void parse_args();
	void run_loader(); // loader thread
	void build_godot_side(); // main thread, once after the publish
	void update_view();

	godot::NodePath session_path_ = godot::NodePath("../Session");
	Bf3LevelSession *session_ = nullptr;
	std::string mode_override_;
	int size_px_ = 0;
	bool enabled_ = true;
	bool north_up_ = false;
	std::string shots_dir_;
	bool check_ = false;

	std::thread loader_;
	std::atomic<bool> stop_{ false };
	std::atomic<bool> published_{ false };
	mutable std::mutex mutex_;
	std::shared_ptr<const MinimapAssets> assets_;

	bool drawn_ = false;
	godot::Ref<godot::ImageTexture> satellite_;
	godot::Ref<godot::ImageTexture> combat_area_;
	godot::Ref<godot::ImageTexture> detail_;
	godot::Ref<godot::ImageTexture> atlas_;
	Bf3MinimapControl *control_ = nullptr;
	Bf3WorldIcons *icons_ = nullptr;
	bf3::core::ui::MinimapView view_;
	std::unique_ptr<MinimapCheck> check_runner_;
};

} // namespace bf3::godot_side
