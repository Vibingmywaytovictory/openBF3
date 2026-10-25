#pragma once

#include "game_crowd.h"

#include <godot_cpp/classes/node3d.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Camera3D;
}

namespace bf3::core {
class Cache;
}

namespace bf3::host {
struct LevelDatabase;
struct SharedLevel;
}

namespace bf3::godot_side {

class Bf3LevelSession;

class Bf3TpCrowdCheck : public godot::Node3D {
	GDCLASS(Bf3TpCrowdCheck, godot::Node3D)

public:
	Bf3TpCrowdCheck();
	~Bf3TpCrowdCheck() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _notification(int p_what);

protected:
	static void _bind_methods();

private:
	struct Script;
	void start_loader();
	void scripted_frame(double p_t, std::vector<host::PresentedSoldier> &r_others, std::vector<host::PresentedOtherWeapon> &r_weapons,
			std::vector<host::PresentEvent> &r_events);
	void check_at(double p_t);
	void finish(int p_code);
	void shutdown();

	Bf3LevelSession *session_ = nullptr;
	bool check_ = false;
	std::string shot_dir_;
	bool shots_ = false;
	std::filesystem::path cache_root_;
	bool started_ = false;
	bool built_ = false;
	bool done_ = false;
	bool shut_down_ = false;
	std::thread loader_;
	std::atomic<bool> load_done_{ false };
	std::atomic<bool> stop_{ false };
	std::string load_error_;
	std::shared_ptr<core::Cache> cache_;
	std::unique_ptr<host::LevelDatabase> db_;
	std::shared_ptr<const host::SharedLevel> shared_;
	Bf3GameCrowd crowd_;
	std::unique_ptr<Script> script_;
	godot::Camera3D *camera_ = nullptr;
	double t_ = 0.0;
	double throw_last_on_ = -1.0;
	uint64_t frame_ = 0;
	int shot_index_ = 0;
	double rebases_after_switch_ = 0.0;
	std::vector<std::string> failures_;
	std::vector<std::string> rows_;
};

}
