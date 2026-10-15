#pragma once
// The in-game minimap gates (docs/design/ui-minimap-icons.md §1 G9-G11, §10.4, §13): `-- --minimap-check`, windowed, on
// res://scenes/m1_metro.tscn after LoadStage::Done. Prints one "UI2 CHECK PASS|FAIL <row> <detail>" line per row and
// "UI2 MINIMAP CHECK: PASS (n/n)", then ends the run through Bf3LevelSession::finish (exit 0 or 1).
//
// Rows:
//   minimap.loaded       the assets published without error; satellite / mask / atlas sizes as read (Metro 2048² L8,
//                        128² L8, 256 x 1024 RGBA8)
//   marker.formula       north-up view: the marker's canvas point equals control centre + minimap_project of the own
//                        presented position within 0.5 px; the satellite texel under it (read back from the viewport
//                        image at the marker, through the shader's tint) is the texel world_to_minimap_uv gives for the
//                        spawn point, compared by value within the tint's rounding (G9)
//   marker.rotating      the same in the rotating view after turning the view by 90 degrees (InputMapper::script_view):
//                        an objective that was to the left of the player in north-up shows above the marker
//   marker.pixels        the viewport image: the marker colour at the formula's pixel (+-1 px) in both views
//   icons.minimap        every objective of --minimap-mode drawn at minimap_project (+-0.5 px), clamped ones on the rim
//   icons.world          every objective in front of the camera at Camera3D::unproject_position (+-0.5 px), behind ones
//                        skipped
//   install.unchanged    sizes and mtimes of the 256 index files (the session's snapshot at start)
// Screenshots of each step go to --minimap-shots=<dir> when given (the acceptance run uses
// C:\Users\olku\BF3 reimplementation\openBF3\b\shots\ui2\).

#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3MinimapHud;

class MinimapCheck {
public:
	explicit MinimapCheck(Bf3MinimapHud &p_hud, std::string p_shots_dir);
	// Main thread, every frame after the HUD drew its first frame; true when finished (the session then quits).
	bool step(double p_delta);
	int exit_code() const;

private:
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	Bf3MinimapHud &hud_;
	std::string shots_dir_;
	std::vector<Row> rows_;
	int phase_ = 0;
	int frames_in_phase_ = 0;
};

} // namespace bf3::godot_side
