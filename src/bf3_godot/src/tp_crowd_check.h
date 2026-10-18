#pragma once
// Bf3TpCrowdCheck: tp's check of Bf3GameCrowd in the level (M2a tp; docs/design/m2a-tp.md). Scene
// res://scenes/m2a_tp_crowd.tscn = Node3D root > Session (Bf3LevelSession, Metro TDM) + TpCrowd (this node). Until gx
// wires Bf3GameCrowd into the level session's wave-3 hooks, this node is the crowd's runtime proof: it loads the crowd
// as the session would (M2aLoadInputs: the install, the SharedLevel with its GunplayLevel, the cache), builds it under
// itself and drives it with a scripted M2aFrame (twelve presented soldiers on S3's validated Metro stage: both teams,
// each kit item, the three stances, a transition, a moving crouch, fire, a reload, a death and a respawn, a weapon
// switch; PresentEvents on the render clock), one fixed 1/60 s step per display frame.
//   --tp-check        headless or windowed: the functional rows (drawn soldiers, weapon instances, muzzles and their
//                     effect frames, unknown weapons, the death hide and the respawn, the weapon rebase, no frozen
//                     frame); prints "M2A TP CROWD CHECK: PASS|FAIL" and exits 0 / 1 (77: no usable install)
//   --tp-shots=<dir>  windowed: screenshots of the scripted moments into <dir> (default <repo>/b/shots/tp; refused
//                     inside the install)
//   --walkers=0 --no-hud --bf3-cache=<root>  the session's (read here too: the crowd opens the same cache root)
// The node only uses the session's public accessors (done, failed, install, shared_level, save_screenshot, finish).

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
} // namespace bf3::host

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
	uint64_t frame_ = 0;
	int shot_index_ = 0;
	double rebases_after_switch_ = 0.0;
	std::vector<std::string> failures_;
	std::vector<std::string> rows_;
};

} // namespace bf3::godot_side
