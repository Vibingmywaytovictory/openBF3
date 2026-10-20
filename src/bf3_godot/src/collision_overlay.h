#pragma once

#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/render/collision_prep.h"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Camera3D;
class CanvasLayer;
class Label;
}

namespace bf3::godot_side {

class Bf3CollisionOverlay : public godot::Node3D {
	GDCLASS(Bf3CollisionOverlay, godot::Node3D)

public:
	struct TileMesh {
		godot::RID mesh;
		godot::RID instance;
		int32_t tile_x = 0;
		int32_t tile_z = 0;
		uint8_t layer = 0;
		size_t triangles = 0;
	};

	Bf3CollisionOverlay();
	~Bf3CollisionOverlay() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _unhandled_input(const godot::Ref<godot::InputEvent> &p_event) override;

	godot::String get_status_text() const;

	bool uploaded() const { return collision_ != nullptr && next_upload_ >= prepared_.size(); }

	bool window_ready() const { return uploaded() && window_valid_ && !window_running_; }
	const bf3::render::TerrainWindowStats &window() const { return window_; }
	const bf3::render::CollisionPrepOptions &window_options() const { return window_options_; }
	const bf3::core::level::LevelCollision &collision() const { return *collision_; }
	const std::vector<bf3::render::PreparedCollisionMesh> &prepared() const { return prepared_; }
	const std::vector<TileMesh> &meshes() const { return meshes_; }
	godot::RID material() const { return material_; }
	godot::Camera3D *camera() const { return camera_; }
	const std::string &level_id() const { return level_; }
	const std::string &mode_id() const { return mode_; }
	const bf3::core::InstallReader &install() const { return install_; }
	const bf3::core::InstallSnapshot &install_snapshot_at_start() const { return install_before_; }
	std::shared_ptr<const bf3::core::Install> opened_install() const { return opened_; }
	double load_seconds() const { return load_seconds_; }
	double prepare_seconds() const { return prepare_seconds_; }
	double upload_seconds() const { return upload_seconds_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Shot {
		std::string path;
		std::array<float, 5> camera{};
	};

	void join_loader();
	void join_window();
	void update_window();
	void free_mesh(TileMesh &r_mesh);
	void free_render_objects();
	void build_ui();
	void update_label();
	void create_material();
	void upload_some(bool p_all);
	void place_start_camera();
	void set_camera(const std::array<float, 5> &p_camera);
	void spawn_camera(size_t p_index);
	void top_camera();
	void apply_camera();
	void apply_layers();
	void apply_wire();
	void move_camera(double p_delta);
	bool save_screenshot(const std::string &p_path);
	void finish(int p_code);

	bf3::core::InstallReader install_;
	bf3::core::InstallSnapshot install_before_;
	std::shared_ptr<const bf3::core::Install> opened_;
	std::string level_ = "MP_Subway";
	std::string mode_ = "TeamDeathMatch0";

	std::thread loader_;
	std::atomic<bool> load_done_{ false };
	bf3::core::level::SoupProgress progress_;
	std::atomic<bool> preparing_{ false };
	std::unique_ptr<bf3::core::level::LevelCollision> loaded_;
	std::vector<bf3::render::PreparedCollisionMesh> loaded_meshes_;
	std::string load_error_;
	double loader_prepare_seconds_ = 0.0;

	std::thread window_thread_;
	std::atomic<bool> window_done_{ false };
	bool window_running_ = false;
	bool window_valid_ = false;
	bf3::render::CollisionPrepOptions window_options_;
	bf3::render::TerrainWindowStats window_;
	std::vector<bf3::render::PreparedCollisionMesh> window_loaded_;
	bf3::render::TerrainWindowStats window_loaded_stats_;
	bf3::render::CollisionPrepOptions window_job_options_;
	double window_seconds_ = 0.0;
	size_t window_jobs_ = 0;
	float terrain_radius_ = bf3::render::CollisionPrepOptions().terrain_radius;

	std::unique_ptr<bf3::core::level::LevelCollision> collision_;
	std::vector<bf3::render::PreparedCollisionMesh> prepared_;
	std::vector<TileMesh> meshes_;
	size_t next_upload_ = 0;
	godot::RID shader_;
	godot::RID material_;
	godot::Camera3D *camera_ = nullptr;
	godot::CanvasLayer *ui_layer_ = nullptr;
	godot::Label *label_ = nullptr;

	godot::Vector3 position_;
	float yaw_ = 0.0f;
	float pitch_ = 0.0f;
	float speed_ = 12.0f;
	bool looking_ = false;
	size_t spawn_index_ = 0;
	int wire_mode_ = 1;
	std::array<bool, bf3::render::kCollisionLayers> layers_{ true, true, true, true, true };
	std::string start_camera_;
	std::string start_view_;

	bool check_mode_ = false;
	bool quitting_ = false;
	bool failed_ = false;
	double quit_after_ = 0.0;
	std::string screenshot_path_;
	std::vector<Shot> shots_;
	size_t next_shot_ = 0;
	int hold_frames_ = 0;
	uint64_t load_start_usec_ = 0;
	double load_seconds_ = 0.0;
	double prepare_seconds_ = 0.0;
	double upload_seconds_ = 0.0;
	double upload_wall_seconds_ = 0.0;
	uint64_t upload_start_usec_ = 0;
	double since_upload_ = 0.0;
	uint64_t frames_ = 0;
	std::vector<float> frame_ms_;
	double label_timer_ = 0.0;
};

}
