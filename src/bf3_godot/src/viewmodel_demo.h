#pragma once
// Bf3ViewmodelDemo: the M0 exit demo (plan §6 M0: "M9 1p empty reload plays in Godot from C++ reading the install
// on BF3 arms + M9 meshes with fov_override"). Scene: res://scenes/m9_reload.tscn.
//
// A loader thread reads the install (bf3_render::load_viewmodel: Ant banks, the level superbundle, meshes,
// materials, textures decoded on all cores) while a label shows its progress; the main thread then builds a
// Skeleton3D from the Ant rig, one shared Skin for the arms and the M9, ArrayMeshes and StandardMaterial3Ds with
// fov_override / z_clip_scale, and plays "ReloadEmpty Anim" in a loop with the camera on CameraJoint (turned 180
// deg about Y: Frostbite looks down +Z, Godot cameras down -Z).
//
// User args (after "--"):
//   --quit-after=<s>            quit after s seconds of playback (after loading)
//   --screenshot=<png>          save the viewport when quitting; with --shot-times, the base name of the shots
//   --shot-times=<t,t,...>      clip times (s) to capture: playback holds at each for a few frames, saves
//                               <png stem>_<t>.png, then continues
//   --clip-speed=<x>            playback rate (default 1)
//   --m9-reload-check           headless check: sample the clip, compare the Skeleton3D's global poses with
//                               bf3_anim's model space (and the probe JSON when OPENBF3_ANT_ORACLE is set),
//                               print a PASS/FAIL table, quit 0/1
//   --bf3-install=<dir>         install override, as for GameSession

#include "bf3/anim/pose.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/render/viewmodel_assets.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skin.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Camera3D;
class CanvasLayer;
class Label;
class MeshInstance3D;
class Skeleton3D;
} // namespace godot

namespace bf3::godot_side {

class Bf3ViewmodelDemo : public godot::Node3D {
	GDCLASS(Bf3ViewmodelDemo, godot::Node3D)

public:
	Bf3ViewmodelDemo();
	~Bf3ViewmodelDemo() override;

	void _ready() override;
	void _process(double p_delta) override;

	godot::String get_status_text() const;

	// Used by the reload check.
	bool loaded() const { return assets_ != nullptr && skeleton_ != nullptr; }
	const bf3::render::ViewmodelAssets &assets() const { return *assets_; }
	godot::Skeleton3D *skeleton() const { return skeleton_; }
	godot::Camera3D *camera() const { return camera_; }
	const bf3::core::InstallReader &install() const { return install_; }
	const bf3::core::InstallSnapshot &install_snapshot_at_start() const { return install_before_; }
	// Poses the Skeleton3D (and the camera) at a clip frame; fills r_model with bf3_anim's model-space pose.
	void pose_at_frame(float p_frame, std::vector<bf3::anim::Xform> *r_model = nullptr);
	double load_seconds() const { return load_seconds_; }
	double upload_seconds() const { return upload_seconds_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void join_loader();
	void build_ui();
	void build_scene();
	void place_reference_props();
	void update_label();
	void save_screenshot(const std::string &p_path);
	void finish(int p_code);

	bf3::core::InstallReader install_;
	bf3::core::InstallSnapshot install_before_;
	bf3::render::ViewmodelSpec spec_;

	// Loader thread state.
	std::thread loader_;
	std::atomic<bool> load_done_{ false };
	bf3::render::LoadProgress progress_;
	std::unique_ptr<bf3::render::ViewmodelAssets> loaded_; // written by the loader before load_done_
	std::string load_error_;

	// Built scene.
	std::unique_ptr<bf3::render::ViewmodelAssets> assets_;
	godot::Skeleton3D *skeleton_ = nullptr;
	godot::Ref<godot::Skin> skin_;
	godot::Camera3D *camera_ = nullptr;
	std::vector<godot::MeshInstance3D *> mesh_nodes_;
	godot::CanvasLayer *ui_layer_ = nullptr;
	godot::Label *label_ = nullptr;
	int32_t camera_joint_ = -1;
	bf3::anim::LocalPose pose_;
	bf3::core::ant::ClipPose scratch_;
	std::vector<bf3::anim::Xform> model_;

	// Playback and run control.
	double clip_time_ = 0.0;
	double clip_speed_ = 1.0;
	double play_elapsed_ = 0.0;
	double quit_after_ = 0.0;
	double load_seconds_ = 0.0;
	double upload_seconds_ = 0.0;
	uint64_t load_start_usec_ = 0;
	std::string screenshot_path_;
	std::vector<double> shot_times_;
	size_t next_shot_ = 0;
	int hold_frames_ = 0; // >0: holding the pose for a capture
	bool check_mode_ = false;
	bool quitting_ = false;
	bool failed_ = false;

	// Frame statistics after loading.
	uint64_t frames_ = 0;
	double frame_seconds_ = 0.0;
	std::vector<float> frame_ms_;
	double animate_us_total_ = 0.0;
	double label_timer_ = 0.0;
};

} // namespace bf3::godot_side
