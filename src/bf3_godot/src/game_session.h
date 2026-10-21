#pragma once

#include "bf3/core/install_reader.h"
#include "bf3/host/loopback_client.h"
#include "bf3/host/server_host.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <memory>
#include <string>
#include <vector>

namespace godot {
class CanvasLayer;
class Label;
class MeshInstance3D;
}

namespace bf3::godot_side {

class S0SelfTest;

class GameSession : public godot::Node3D {
	GDCLASS(GameSession, godot::Node3D)

public:
	GameSession();
	~GameSession() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _physics_process(double p_delta) override;

	bool start_server();
	void stop_server();
	bool is_server_running() const;
	int64_t get_server_ticks() const;
	godot::String get_status_text() const;

	void set_autostart(bool p_value) { autostart_ = p_value; }
	bool get_autostart() const { return autostart_; }
	void set_dynamic_bodies(int p_value) { dynamic_bodies_ = p_value; }
	int get_dynamic_bodies() const { return dynamic_bodies_; }
	void set_server_bodies_offset(const godot::Vector3 &p_value) { server_bodies_offset_ = p_value; }
	godot::Vector3 get_server_bodies_offset() const { return server_bodies_offset_; }

	const bf3::core::InstallReader &install() const { return install_; }
	const bf3::core::InstallSnapshot &install_snapshot_at_start() const { return install_before_; }
	bf3::host::ServerStats server_stats() const;
	bf3::host::LoopbackClientStats client_stats() const;
	uint64_t physics_frames() const { return physics_frames_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void build_overlay();
	void build_server_body_nodes();
	void update_server_body_nodes();
	void update_overlay();
	void save_screenshot(const std::string &p_path);
	void save_screenshot_and_quit();

	bool autostart_ = true;
	int dynamic_bodies_ = 8;
	godot::Vector3 server_bodies_offset_ = godot::Vector3(3.0f, 0.0f, 0.0f);

	bf3::core::InstallReader install_;
	bf3::core::InstallSnapshot install_before_;
	std::unique_ptr<bf3::host::ServerHost> server_;
	std::unique_ptr<bf3::host::LoopbackClient> client_;
#if OPENBF3_BUILD_CHECKS
	std::unique_ptr<S0SelfTest> selftest_;
#endif

	godot::CanvasLayer *overlay_layer_ = nullptr;
	godot::Label *overlay_ = nullptr;
	std::vector<godot::MeshInstance3D *> body_nodes_;
	std::vector<bf3::sim::BodyTransform> body_scratch_;

	std::string screenshot_path_;
	double quit_after_ = 0.0;
	double elapsed_ = 0.0;
	double overlay_timer_ = 0.0;
	uint64_t frames_ = 0;
	uint64_t physics_frames_ = 0;
	bool quitting_ = false;
	bool mid_screenshot_done_ = false;
};

}
