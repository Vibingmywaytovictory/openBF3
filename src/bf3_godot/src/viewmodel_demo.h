#pragma once

#include "bf3/anim/pose.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/render/viewmodel_assets.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skin.hpp>

#include <atomic>
#include <filesystem>
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
}

namespace bf3::godot_side {

class Bf3ViewmodelDemo : public godot::Node3D {
	GDCLASS(Bf3ViewmodelDemo, godot::Node3D)

public:
	Bf3ViewmodelDemo();
	~Bf3ViewmodelDemo() override;

	void _ready() override;
	void _process(double p_delta) override;

	godot::String get_status_text() const;

	bool loaded() const { return assets_ != nullptr && skeleton_ != nullptr; }
	const bf3::render::ViewmodelAssets &assets() const { return *assets_; }
	godot::Skeleton3D *skeleton() const { return skeleton_; }
	godot::Camera3D *camera() const { return camera_; }
	const bf3::core::InstallReader &install() const { return install_; }
	const bf3::core::InstallSnapshot &install_snapshot_at_start() const { return install_before_; }
	const bf3::render::ViewmodelSpec &spec() const { return spec_; }

	const std::string &cache_summary() const { return cache_summary_; }
	bool cache_used() const { return cache_used_; }

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

	std::thread loader_;
	std::atomic<bool> load_done_{ false };
	bf3::render::LoadProgress progress_;
	std::unique_ptr<bf3::render::ViewmodelAssets> loaded_;
	std::string load_error_;
	std::filesystem::path cache_root_;
	bf3::core::PathGuard cache_guard_;
	std::string cache_summary_;
	bool cache_used_ = false;

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
	int hold_frames_ = 0;
	bool check_mode_ = false;
	bool quitting_ = false;
	bool failed_ = false;

	uint64_t frames_ = 0;
	double frame_seconds_ = 0.0;
	std::vector<float> frame_ms_;
	double animate_us_total_ = 0.0;
	double label_timer_ = 0.0;
};

}
