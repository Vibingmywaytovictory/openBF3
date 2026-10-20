#pragma once

#include "fx_renderer.h"

#include "bf3/fx/effect_library.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/impact_table.h"
#include "bf3/fx/simulator.h"

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
namespace gameplay {
struct WeaponSources;
}
}

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::godot_side {

class Bf3LevelSession;

class Bf3FxDemo : public godot::Node3D {
	GDCLASS(Bf3FxDemo, godot::Node3D)

public:
	Bf3FxDemo();
	~Bf3FxDemo() override;

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Loaded;
	struct Pose {
		godot::Vector3 position, target;
	};
	struct Shot {
		std::string name;
		double at = 0;
		int kind = 0;
		double advance = 0;
	};

	void parse_args();
	void start_loader();
	bool build(std::string *r_error);
	void find_surfaces();
	void set_pose(const Pose &p_pose);
	bf3::fx::FxView view() const;
	bf3::fx::Transform muzzle_transform() const;
	void start_effect(const std::shared_ptr<const bf3::fx::EffectDef> &p_def, const bf3::fx::Transform &p_t, bool p_first_person);
	void fire_muzzle();
	void start_impact(int p_which);
	void start_light_stress();
	void frame_checks();
	void report_and_finish();
	void finish(int p_code);
	void shutdown();

	Bf3LevelSession *session_ = nullptr;
	godot::Camera3D *camera_ = nullptr;
	Bf3FxRenderer *renderer_ = nullptr;
	std::thread loader_;
	std::unique_ptr<Loaded> loaded_;
	std::unique_ptr<bf3::sim::CollisionWorld> world_collision_;
	std::unique_ptr<bf3::fx::FxWorld> world_;
	std::filesystem::path cache_root_;
	bool no_cache_ = false;
	bool check_ = false;
	int debug_view_ = 0;
	std::string shot_dir_;
	bool shot_dir_ok_ = false;
	bool started_ = false, built_ = false, finished_ = false, shut_down_ = false;

	Pose main_pose_;
	godot::Vector3 concrete_point_, concrete_normal_, brick_point_, brick_normal_, smoke_point_, oil_point_;
	bool concrete_found_ = false, brick_found_ = false;
	std::string concrete_line_, brick_line_;
	Pose oil_pose_;
	bool flash_present(std::string *r_where = nullptr) const;
	std::string flash_line() const;
	void frame_oilfire();
	double oil_age_ = 0;
	std::shared_ptr<const bf3::fx::EffectDef> impact_concrete_, impact_brick_;

	double t_ = 0;
	double next_muzzle_ = 0.5, next_impact_ = 1.0;
	int impact_turn_ = 0;
	bool frozen_ = false;
	int settle_ = 0;
	int shot_retries_ = 0;
	bool oil_framed_ = false, smoke_started_ = false;
	std::string oil_frame_line_;
	std::vector<Shot> shots_;
	size_t shot_index_ = 0;
	double shot_advance_left_ = 0;
	bool shot_active_ = false;
	std::vector<std::string> shot_lines_;
	uint32_t shots_ok_ = 0;
	bool stress_started_ = false;

	uint64_t frames_ = 0;
	uint64_t count_mismatch_frames_ = 0;
	uint64_t fp_mismatch_frames_ = 0;
	uint64_t fp_frames_ = 0;
	uint32_t max_lights_ = 0;
	uint32_t max_light_candidates_ = 0;
	uint32_t max_instances_ = 0;
	uint64_t muzzle_starts_ = 0, impact_starts_ = 0;
	int oil_count_at_15_ = -1;
	int oil_drawn_at_15_ = -1;
	std::vector<double> fx_ms_;
	std::vector<double> step_ms_, prep_ms_, upload_ms_;
	uint32_t atlas_entries_ = 0, atlas_uploaded_ = 0;
	std::string atlas_line_, cache_line_, load_line_;
	std::vector<std::string> notes_;
	std::vector<std::string> report_;
};

}
