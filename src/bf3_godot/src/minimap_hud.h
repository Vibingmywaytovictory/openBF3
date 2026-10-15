#pragma once
// Bf3MinimapHud: BF3's minimap and the flag / HQ world icons in the running M1 game (docs/design/ui-minimap-icons.md
// §10; plan §3.8, D16). A CanvasLayer added to res://scenes/m1_metro.tscn as its own node beside the Session: it reads
// the session through Bf3LevelSession's public accessors and changes nothing there (the lk and s3 tracks edit
// level_session in parallel).
//
// When the node is inert (design §10.1). It parses the user args itself (OS::get_cmdline_user_args; the session has
// no accessor for them) and creates nothing, starts no loader and draws nothing when any of these holds:
//   --headless or the session has no scene (scene_enabled() false: --m1-check), --no-minimap, --no-hud, or one of the
//   session's measurement / screenshot runs: --m1-load-check (godot.m1_load reports frames.after_done and the peak
//   working set, which a second Metro load would skew), --shot-spawns, --shots (other tracks' reference shots stay
//   free of the minimap).
// --minimap-check and --minimap-shots switch it back on for those runs' own purpose only (godot.ui2 never combines
// them with --m1-load-check).
//
// Lifetime:
//   _ready: finds the session (session_path, default "../Session"); returns when inert.
//   loader std::thread (no Godot API), started from _process once the session reports done(): opens its own Install
//     from the session's InstallReader (0.04 s), then ui::load_minimap_level with kMinimapHudLoaderThreads (2) parse
//     lanes and the thread at below-normal priority (plan §2.3: loader threads stay off the frame's and the 60 Hz
//     server's cores), ui::load_minimap_images (satellite, detail SDF, combat-area SDFs; L8), the satellite's mip
//     chain, Win32/UI: read_hud_icon_atlas + load_hud_icon_texture (RGBA8). Not cached (design §8). Starting after
//     LoadStage::Done keeps the M1 load gates (E4 / E5) free of this work. Publishes once (mutex + release flag).
//   main thread, after the publish: Image::create_from_data (FORMAT_L8 with the box-filtered chain, FORMAT_RGBA8), the
//     ShaderMaterial (minimap_control.h), the two Controls; then the CPU pixels (MinimapPixels, ~10 MB on Metro) are
//     released: only MinimapAssets (no pixels) stays, the check reads the viewport instead. Every frame: the own
//     soldier's presented position (fp_camera presented_eye of the session's PresentationFrame) and the view yaw
//     (InputMapper::view, the local mouse state, so the map turns without network latency) -> MinimapView ->
//     queue_redraw.
//   NOTIFICATION_PREDELETE: sets the stop flag and joins the loader. The flag is checked between steps only, so a quit
//     during the load waits for the current step (at most one load_level, ~0.4 s warm). The node sits after the
//     Session in the scene, so Godot frees it first and the join runs before the session goes away.
//
// User args (after "--"; read by this node only):
//   --no-minimap                     do not create the minimap (also inert under --no-hud and the runs listed above)
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
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3MinimapControl;
class Bf3WorldIcons;
class MinimapCheck;

// The local player's team in the M1 game (docs/design/m1.md §8.2: "M1 puts the local player on Team 1"); the icon
// states are relative to it (hud_icons.h objective_icon / soldier_icon). Replaced by the client's team in M2b.
inline constexpr int32_t kM1LocalTeam = 1;

// What the loader publishes and the HUD keeps (immutable after the publish; no pixels).
struct MinimapAssets {
	bf3::core::ui::MinimapLevel level;
	bf3::core::ui::HudIconAtlas atlas;
	uint32_t satellite_side = 0; // as read (Metro 2048)
	uint32_t combat_area_side = 0; // 128
	uint32_t atlas_width = 0; // 256
	uint32_t atlas_height = 0; // 1024
	double seconds = 0.0; // loader wall time (G14, minimap.loaded)
	int64_t peak_working_set_delta = 0; // process PeakWorkingSetSize after the loader minus before it, bytes (G14)
	std::string error; // non-empty: nothing to draw (logged once)
};

// The pixels, handed to the main thread once and released after the Godot upload.
struct MinimapPixels {
	bf3::core::ui::MinimapImages images;
	std::vector<bf3::core::ui::L8Image> satellite_mips; // the box-filtered chain, mip 0 first
	bf3::core::ui::Rgba8Image atlas_image;
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
	// Re-centres the minimap's view on a world point (x, z) instead of the own soldier (the check's HQ / flag shots);
	// cleared by clear_center_override. The soldier and the session are not touched.
	void set_center_override(float p_x, float p_z);
	void clear_center_override();

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
	std::unique_ptr<MinimapPixels> pixels_; // under mutex_ until build_godot_side takes and frees it
	bool center_override_ = false;
	float center_override_x_ = 0.0f;
	float center_override_z_ = 0.0f;

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
