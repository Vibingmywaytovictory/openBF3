#pragma once
// The M2a minimal HUD check (docs/design/m2a.md §1 E10, §15 row hud; as built: docs/design/m2a-hud.md):
// res://scenes/m2a_hud.tscn (Bf3Localisation, Bf3UiAssets, Bf3HudCheck).
//
//   windowed  -- --hud-check [--hud-shots=<dir>]   rows, last line "M2A HUD CHECK: PASS (n/n)", exit 0 / 1
//
// Bf3HudCheck sets Bf3UiAssets' atlas movies to Bf3Hud::required_atlas_movies() in _ready (before the assets load),
// resolves the 12 kit items of the M2a loadouts on its own thread (open_install_weapons, the catalog,
// make_weapon_table, make_hud_static), applies the item index's names (the M67), then drives a Bf3Hud inside a
// SubViewport at 1280 x 720, 1920 x 1080 and 2560 x 1440 (shown scaled in the window) with a scripted model: per
// kit item 96 frames of a 1/64 s model clock (deploy, fire with the magazine and spread changing, ADS at a 40 degree
// vertical FOV, a reload, a 75 degree hip segment with health 67 and pose changes, then dead), own and other Hit
// events. Each frame the script's inputs go through fill_hud into the HudModel; on the next frame every widget's draw
// record is compared with what the script expects (independently of the model: the script's magazine, reserve,
// spread, FOV, hits, names):
//   ammo      visible iff alive with an ammo weapon; magazine / reserve texts; the $HUD_medium FontFile; font sizes
//             round(slot size x scale); the clip's right end at the Ammo_01 slot (stage x 1194 +- 2)
//   crosshair visible iff alive and not ADS; gap = (h / 2) tan(spread) / tan(vfov / 2) +- 0.5 px
//   hit       visible iff the last own Hit was less than kHitIndicatorSeconds ago; shown on the frame its event is
//             drained (latency 0 frames); one rising edge per own Hit; other Hits never show it
//   name      the localised name of the item (the 12 expected strings); its _small icon drawn
//   health    visible iff alive; round(health)
//   serial    every widget drew the frame's model
// Shot frames hold their state for 2 frames, then read the SubViewport back: crosshair inner edge within 1 px of
// the expected gap (hip), no crosshair pixels in ADS, hit indicator pixels present / absent, ammo digit and icon
// pixels present, and save <dir>/hud_<w>x<h>_<label>.png (hip, hit, ads, reload at each size; one per kit item at
// 1920 x 1080). The shots show BF3's fonts and icons: they go to a gitignored directory outside the worktree
// (ctest: $ENV{OPENBF3_HUD_SHOTS}, default C:\Users\olku\BF3 reimplementation\openBF3\b\shots\hud), never the repo.
// Last row install.unchanged (Bf3Localisation's snapshot).

#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture_rect.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bf3::godot_side {

class Bf3Hud;

class Bf3HudCheck : public godot::Node {
	GDCLASS(Bf3HudCheck, godot::Node)

public:
	Bf3HudCheck();
	~Bf3HudCheck() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

protected:
	static void _bind_methods() {}

private:
	struct Work;
	struct Frame;
	struct Tally;
	void start_work();
	void build_script();
	void verify_frame(const Frame &p_frame);
	void check_shot(const Frame &p_frame);
	void finish();
	void add_row(const std::string &p_name, bool p_pass, const std::string &p_detail);

	bool check_ = false;
	std::string shot_dir_;
	int state_ = 0;
	godot::SubViewport *viewport_ = nullptr;
	godot::ColorRect *background_ = nullptr;
	godot::TextureRect *view_ = nullptr;
	Bf3Hud *hud_ = nullptr;
	std::unique_ptr<Work> work_;
	std::thread worker_;
	std::atomic<bool> work_done_{ false };
	std::vector<Frame> frames_;
	std::vector<Tally> tallies_; // per resolution
	size_t next_ = 0; // the next script frame to publish
	int settle_ = 0;
	int hold_ = 0;
	bool pending_verify_ = false;
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};
	std::vector<Row> rows_;
	int exit_code_ = 0;
	double started_s_ = 0;
};

} // namespace bf3::godot_side
