#pragma once
// Bf3M2aViewmodelDemo: the vm track's demo and check scene (docs/design/m2a-vm.md; m2a.md §11, E19, E5, E11).
// Scene: res://scenes/m2a_viewmodel.tscn.
//
// A loader thread reads the install (read-only): the shared viewmodel sources, the US and RU viewmodel sets of the
// 8 default loadouts (render::team_viewmodel_set), the weapon table of their 12 variants (bf3_sim
// open_install_weapons), and runs the scripted timeline of every kit (vm_script.h). The main thread builds one
// Bf3Viewmodel per team and a Camera3D, then plays the timelines through Bf3Viewmodel::present.
//
// User args (after "--"):
//   --vm-check               headless: every kit at 60 and 144 display frames per second, the E19 / E5 / E11 rows as
//                            PASS / FAIL lines, "M2A VIEWMODEL CHECK: PASS (n/n)", quit 0 / 1
//   --vm-shots=<dir>         windowed: play the kits at 60 fps and save the look-and-feel captures (hip, ADS,
//                            reload mid-clip, throw, knife) as PNGs into <dir>, then quit
//   --kits=<i,j,...>         only these kits (0-3 US assault engineer support recon, 4-7 RU)
//   --quit-after=<s>         play mode: quit after s seconds
//   --bf3-install=<dir>      install override; --bf3-cache=<dir> / --no-cache as the M9 demo
// Without --vm-check / --vm-shots the kits play in a loop with a status label.

#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/path_guard.h"
#include "bf3/render/viewmodel_set.h"
#include "bf3/sim/weapons/weapon_sim_def.h"
#include "vm_script.h"

#include <godot_cpp/classes/node3d.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Camera3D;
class Label;
} // namespace godot

namespace bf3::godot_side {

class Bf3Viewmodel;

class Bf3M2aViewmodelDemo : public godot::Node3D {
	GDCLASS(Bf3M2aViewmodelDemo, godot::Node3D)

public:
	Bf3M2aViewmodelDemo();
	~Bf3M2aViewmodelDemo() override;

	void _ready() override;
	void _process(double p_delta) override;

	// Used by the check (m2a_viewmodel_check.cpp).
	struct Kit {
		size_t index = 0;
		int team = 1;
		std::array<int, vm_script::kKitItems> item{}; // the team set's item index per sim slot
		vm_script::KitRun run;
	};
	const std::vector<Kit> &kits() const { return kits_; }
	const bf3::render::ViewmodelSet &set(int p_team) const { return p_team == 2 ? *set_ru_ : *set_us_; }
	Bf3Viewmodel *viewmodel(int p_team) const;
	godot::Camera3D *camera() const { return camera_; }
	const bf3::sim::weapons::WeaponTable &table() const { return *table_; }
	const bf3::anim::ViewmodelTuning &tuning() const { return tuning_; }
	const std::string &tuning_source() const { return tuning_source_; }
	// Presents kit p_kit at script time p_t (cues from (p_prev_t, p_t]) with display frame time p_dt.
	void present_kit(size_t p_kit, double p_prev_t, double p_t, float p_dt);
	// The view the last present_kit used.
	const vm_script::View &last_view() const { return last_view_; }
	void set_tuning(const bf3::anim::ViewmodelTuning &p_tuning);
	double load_seconds() const { return load_seconds_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void join_loader();
	void build_scene();
	void build_props();
	void update_label(const std::string &p_extra = std::string());
	bool save_png(const std::string &p_path);
	void finish(int p_code);
	void begin_kit(size_t p_kit);
	void shots_step();

	bf3::core::InstallReader install_;
	bf3::core::InstallSnapshot install_before_;
	bf3::anim::ViewmodelTuning tuning_;
	std::string tuning_source_;
	std::vector<size_t> kit_filter_;

	// Loader thread state (written before load_done_).
	std::thread loader_;
	std::atomic<bool> load_done_{ false };
	bf3::render::LoadProgress progress_;
	std::string load_error_;
	std::filesystem::path cache_root_;
	bf3::core::PathGuard cache_guard_;
	std::unique_ptr<bf3::render::ViewmodelSources> sources_;
	std::unique_ptr<bf3::render::ViewmodelSet> set_us_, set_ru_;
	std::shared_ptr<const bf3::sim::weapons::WeaponTable> table_;
	std::vector<Kit> kits_;
	std::vector<std::string> load_notes_;

	// Scene.
	bool built_ = false;
	Bf3Viewmodel *vm_us_ = nullptr;
	Bf3Viewmodel *vm_ru_ = nullptr;
	godot::Camera3D *camera_ = nullptr;
	godot::Label *label_ = nullptr;
	vm_script::View last_view_;
	int shown_team_ = 0;

	// Run control.
	bool check_mode_ = false;
	std::string shots_dir_;
	double quit_after_ = 0;
	double played_ = 0;
	size_t kit_cursor_ = 0; // index into the kits to play
	double script_t_ = 0;
	struct Capture {
		double time_s;
		std::string name;
	};
	std::vector<Capture> captures_;
	size_t next_capture_ = 0;
	int hold_frames_ = 0;
	uint32_t shots_saved_ = 0;
	bool failed_ = false;
	bool quitting_ = false;
	double load_seconds_ = 0;
	uint64_t load_start_usec_ = 0;
};

// The headless check (m2a_viewmodel_check.cpp): returns the exit code.
int run_m2a_viewmodel_check(Bf3M2aViewmodelDemo &p_demo);

} // namespace bf3::godot_side
