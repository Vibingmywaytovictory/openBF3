#pragma once
// The in-game minimap gates (docs/design/ui-minimap-icons.md §1 G9-G11, §10.4, §13): `-- --minimap-check`, windowed, on
// res://scenes/m1_metro.tscn after LoadStage::Done. ctest godot.ui2 always adds --minimap-mode=ConquestSmall0 (the M1
// game's TDM has no objectives). Prints one "UI2 CHECK PASS|FAIL <row> <detail>" line per row and
// "UI2 MINIMAP CHECK: PASS (n/n)", then ends the run through Bf3LevelSession::finish (exit 0 or 1).
//
// The evidence is the viewport image, never the control's own record of where it drew: the control's last_marker /
// last_icons only say where to look. Read-back: get_viewport()->get_texture()->get_image() two frames after a change
// (so the frame with the change has been presented), pixels addressed through Bf3MinimapControl::canvas_to_viewport;
// a sample point is the centre of a viewport pixel mapped back to control-local units with the inverse transform.
//
// Rows:
//   minimap.loaded       the assets published without error; satellite / mask / atlas sizes as read (Metro 2048² L8,
//                        128² L8, 256 x 1024 RGBA8); the loader's wall time and process peak-working-set delta (G14)
//   readback.calibration an opaque ColorRect of Color8(17, 34, 51) drawn by the check (whole canvas-unit origin, at
//                        least 8 x 8 viewport pixels) reads back as exactly (17, 34, 51) at the 4 x 4 pixels around its
//                        centre: the viewport image holds the shader's bytes unchanged (no colour conversion,
//                        debanding or blending), which the UV read-back depends on
//   marker.spawn         the first drawn frame after Done (no input yet): the own presented position (x, z) is within
//                        0.5 m of one of shared_level()->spawns, the spawn the session used. Then the view is frozen:
//                        the minimap re-centred there (set_center_override), the mapper scripted, the yaw rounded to a
//                        multiple of 90 degrees (yaw0); the north-up frames are taken at yaw0 + pi/2
//   map.uv.north_up      north-up view, icon item hidden, debug_uv on: at K = 49 sample pixels (the centre, 3 rings of
//                        16 at 25 / 50 / 85 % of the radius, inside the disc and with an expected UV inside [0, 1]) the
//                        decoded UV equals minimap_expected_uv(view, mapping, sample) within kMinimapUvCheckTexels
//                        (1 satellite texel) per axis; for each of the 7 other mapping variants the largest error over
//                        the samples exceeds kMinimapUvMirrorTexels (10 texels). The centre sample is the marker's
//                        point, so this is the task's check: the map under the marker is world_to_minimap_uv of the
//                        spawn position
//   map.uv.rotating      the same after a scripted 90-degree turn (InputMapper::script_view, yaw0 + pi) in the rotating
//                        view; additionally the samples straight above the centre whose expected UV is inside the
//                        image (at least one) decode to world points within 2 degrees of the view direction
//   marker.pixels        normal shading, icons visible: the pixels that differ from the icons-hidden frame near the
//                        centre have their bounding-box centre within 1 canvas unit of the control centre, both views
//   icons.minimap        --minimap-mode=ConquestSmall0, both views: for each of the 5 objectives whose
//                        minimap_project point lies inside the disc, the debug_uv read-back at the icon item's drawn
//                        point decodes to world_to_minimap_uv(objective) within 1 texel (rim-clamped ones: on the rim
//                        within 1 unit, in the objective's direction within 2 degrees); the icons-visible frame differs
//                        from the hidden frame inside each drawn icon's rect; each drawn entry and state equal
//                        objective_icon(hq, owner, kM1LocalTeam)
//   icons.world          after a level turn towards the objectives' centroid, each objective not behind the camera: Bf3WorldIcons' centre equals the check's own projection
//                        (camera projection x inverse camera transform, multiplied out here) of position +
//                        (0, VerticalOffset 5.5, 0), plus PixelOffset (0, -28), within 0.5 canvas units; behind ones not
//                        drawn; the image differs from a no-world-icons frame inside each on-screen icon's rect
//   install.unchanged    sizes and mtimes of the 256 index files (the session's snapshot at start)
// Screenshots (G11) go to --minimap-shots=<dir> when given (the acceptance run uses
// C:\Users\olku\BF3 reimplementation\openBF3\b\shots\ui2\): at the spawn rotating and north-up, then with the minimap
// re-centred on an HQ and on a flag (Bf3MinimapHud::set_center_override: the HUD's MinimapView only, the soldier does
// not move), rotating and north-up; the debug_uv and icons-hidden frames are never saved.

#include "bf3/core/ui/minimap.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <string>
#include <vector>

namespace godot {
class CanvasLayer;
class ColorRect;
} // namespace godot

namespace bf3::godot_side {

class Bf3MinimapHud;

inline constexpr int kMinimapCheckRingSamples = 16;
inline constexpr int kMinimapCheckRings = 3;

class MinimapCheck {
public:
	// p_rows false: --minimap-shots without --minimap-check (the screenshots only, no rows; the run then ends with 0).
	MinimapCheck(Bf3MinimapHud &p_hud, std::string p_shots_dir, bool p_rows = true);
	// Main thread, every frame after the HUD drew its first frame; true when finished (the session then quits).
	bool step(double p_delta);
	int exit_code() const;

private:
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	// One view's frames (north-up or rotating): debug UV, normal with the icon item hidden, marker only, all icons.
	struct ViewFrames {
		godot::Ref<godot::Image> uv;
		godot::Ref<godot::Image> hidden;
		godot::Ref<godot::Image> marker;
		godot::Ref<godot::Image> full;
		bf3::core::ui::MinimapView view;
	};
	void add(const std::string &p_name, bool p_pass, const std::string &p_detail);
	godot::Ref<godot::Image> grab() const;
	void row_calibration(const godot::Ref<godot::Image> &p_image);
	void row_map_uv(const char *p_name, const ViewFrames &p_frames);
	void row_marker_pixels(const ViewFrames &p_north, const ViewFrames &p_rotating);
	void row_icons_minimap(const ViewFrames &p_north, const ViewFrames &p_rotating);
	void evaluate_icons(const ViewFrames &p_frames, std::string &r_detail, bool &r_pass, size_t &r_checked);
	void row_icons_world(const godot::Ref<godot::Image> &p_hidden, const godot::Ref<godot::Image> &p_visible);
	void finish_rows();

	Bf3MinimapHud &hud_;
	std::string shots_dir_;
	bool rows_enabled_ = true;
	std::vector<Row> rows_;
	int phase_ = 0;
	int frames_in_phase_ = 0;
	int wait_ = 0;
	float yaw0_ = 0.0f;
	float pitch0_ = 0.0f;
	float world_yaw_ = 0.0f;
	godot::CanvasLayer *calibration_layer_ = nullptr;
	godot::ColorRect *calibration_ = nullptr;
	godot::Vector2i calibration_px_;
	ViewFrames north_;
	ViewFrames rotating_;
	godot::Ref<godot::Image> world_hidden_;
	// Icon positions recorded at the full frame of each view (the control redraws every frame with the same frozen view).
	std::vector<bf3::core::ui::ScreenPoint> north_icons_;
	std::vector<godot::Rect2> north_rects_;
	std::vector<bool> north_clamped_;
	std::vector<bf3::core::ui::ScreenPoint> rotating_icons_;
	std::vector<godot::Rect2> rotating_rects_;
	std::vector<bool> rotating_clamped_;
	size_t shot_index_ = 0;
	size_t shots_saved_ = 0;
	bool shots_failed_ = false;
	bool done_ = false;
	int exit_code_ = 1;
};

} // namespace bf3::godot_side
