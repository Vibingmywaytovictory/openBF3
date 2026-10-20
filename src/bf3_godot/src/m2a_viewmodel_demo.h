#pragma once

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
}

namespace bf3::godot_side {

class Bf3Viewmodel;

class Bf3M2aViewmodelDemo : public godot::Node3D {
	GDCLASS(Bf3M2aViewmodelDemo, godot::Node3D)

public:
	Bf3M2aViewmodelDemo();
	~Bf3M2aViewmodelDemo() override;

	void _ready() override;
	void _process(double p_delta) override;

	struct Kit {
		size_t index = 0;
		int team = 1;
		std::array<int, vm_script::kKitItems> item{};
		vm_script::KitRun run;
	};
	const std::vector<Kit> &kits() const { return kits_; }

	const std::vector<bf3::render::ViewmodelKit> &kit_defs() const { return kit_defs_; }
	const std::string &kit_defs_difference() const { return kit_defs_difference_; }
	const bf3::render::ViewmodelSet &set(int p_team) const { return p_team == 2 ? *set_ru_ : *set_us_; }
	Bf3Viewmodel *viewmodel(int p_team) const;
	godot::Camera3D *camera() const { return camera_; }
	const bf3::sim::weapons::WeaponTable &table() const { return *table_; }
	const bf3::anim::ViewmodelTuning &tuning() const { return tuning_; }
	const std::string &tuning_source() const { return tuning_source_; }

	void present_kit(size_t p_kit, double p_prev_t, double p_t, float p_dt);

	const vm_script::View &last_view() const { return last_view_; }
	void set_tuning(const bf3::anim::ViewmodelTuning &p_tuning);
	double load_seconds() const { return load_seconds_; }
	bool knife_dog_tags() const { return knife_dog_tags_; }

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
	std::vector<bf3::render::ViewmodelKit> kit_defs_;
	std::string kit_defs_difference_;
	std::vector<std::string> load_notes_;

	bool built_ = false;
	Bf3Viewmodel *vm_us_ = nullptr;
	Bf3Viewmodel *vm_ru_ = nullptr;
	godot::Camera3D *camera_ = nullptr;
	godot::Label *label_ = nullptr;
	vm_script::View last_view_;
	int shown_team_ = 0;

	bool check_mode_ = false;
	bool knife_dog_tags_ = false;
	std::string shots_dir_;
	double quit_after_ = 0;
	double played_ = 0;
	size_t kit_cursor_ = 0;
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

int run_m2a_viewmodel_check(Bf3M2aViewmodelDemo &p_demo);

}
