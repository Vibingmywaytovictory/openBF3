#pragma once

#include "game_fx.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::godot_side {

class Bf3LevelSession;

class Bf3Fx3Demo : public godot::Node3D {
	GDCLASS(Bf3Fx3Demo, godot::Node3D)

public:
	Bf3Fx3Demo();
	~Bf3Fx3Demo() override;

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Loaded;
	struct Surface {
		bool found = false;
		godot::Vector3 point, normal;
		uint8_t material = 0;
		std::string name;
		std::string effect;
		float distance = 0;
	};
	struct Shot {
		std::string name;
		double at = 0;
		int kind = 0;
		double advance = 0;
	};

	void start_loader();
	bool build(std::string *r_error);
	void find_surfaces();
	void place_soldier();
	void set_pose(const godot::Vector3 &p_position, const godot::Vector3 &p_target);
	void own_shot(bool p_tracer);
	void impact(const Surface &p_surface, int p_index);
	void soldier_hit();
	void remote_burst();
	bool flash_present() const;
	void step_fx(double p_dt);
	void report_and_finish();
	void finish(int p_code);
	void shutdown();

	Bf3LevelSession *session_ = nullptr;
	std::unique_ptr<Loaded> loaded_;
	std::thread loader_;
	std::atomic<bool> stop_{ false };
	std::unique_ptr<sim::CollisionWorld> collision_;
	godot::Camera3D *camera_ = nullptr;
	godot::Node3D *soldier_ = nullptr;
	bool check_ = false;
	bool no_cache_ = false;
	bool started_ = false;
	bool built_ = false;
	bool finished_ = false;
	bool shut_down_ = false;
	std::string shot_dir_;
	bool shot_dir_ok_ = false;
	std::filesystem::path cache_root_;
	uint16_t rifle_ = 0xFFFF;
	uint8_t bullet_ = 1;
	Surface concrete_, metal_, dirt_;
	godot::Vector3 main_position_, main_target_;
	std::string main_pose_line_;
	godot::Vector3 soldier_point_, soldier_normal_;
	bool soldier_found_ = false;
	std::vector<bf3::host::PresentEvent> events_;
	std::vector<Shot> shots_;
	size_t shot_index_ = 0;
	bool shot_active_ = false;
	bool frozen_ = false;
	int settle_ = 0;
	int flash_retries_ = 0;
	double shot_left_ = 0;
	double t_ = 0;
	double next_shot_ = 0;
	uint32_t shot_id_ = 0;
	uint32_t remote_id_ = 0;
	uint64_t frames_ = 0;
	uint32_t shots_ok_ = 0;
	uint64_t fp_frames_ = 0;
	uint64_t mismatch_frames_ = 0;
	uint32_t max_decals_ = 0;
	std::vector<std::string> shot_lines_;
	std::vector<std::string> notes_;
	std::vector<double> fx_ms_;
};

}
