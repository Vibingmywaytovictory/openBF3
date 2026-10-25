#pragma once

#include "bf3/fx/fx_types.h"

#include <cstdint>
#include <type_traits>

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::godot_side {

struct DeathCameraParams {
	float seconds = 3.9f;
	float orbit_seconds = 1.0f;
	float turn_seconds = 0.75f;
	float back_m = 2.5f;
	float up_m = 1.6f;
	float eye_height_m = 0.4f;
	float camera_radius_m = 0.2f;

	float killer_height_m = 1.2f;
};
static_assert(std::is_trivially_copyable_v<DeathCameraParams>);

struct DeathCameraStart {
	fx::Vec3 corpse{};
	float yaw_rad = 0.0f;
	fx::Vec3 eye{};
	uint8_t killer_slot = 0xFF;
	bool killer_camera = true;
};
static_assert(std::is_trivially_copyable_v<DeathCameraStart>);

struct DeathCameraView {
	bool active = false;
	bool on_killer = false;
	bool finished = false;
	fx::Transform camera;
};
static_assert(std::is_trivially_copyable_v<DeathCameraView>);

struct DeathCameraStats {
	uint64_t views = 0;
	uint64_t killer_views = 0;
	uint64_t pulled_in = 0;
	uint64_t bad_inputs = 0;
};
static_assert(std::is_trivially_copyable_v<DeathCameraStats>);

class Bf3DeathCamera {
public:
	explicit Bf3DeathCamera(const DeathCameraParams &p_params = {});

	void begin(const DeathCameraStart &p_start, double p_now_s);

	void end();
	bool active() const;

	DeathCameraView step(double p_now_s, const fx::Vec3 *p_killer, const sim::CollisionWorld *p_world);
	const DeathCameraParams &params() const { return params_; }
	DeathCameraStats stats() const { return stats_; }

private:
	DeathCameraParams params_;
	DeathCameraStart start_{};
	double start_s_ = 0.0;
	bool active_ = false;
	bool killer_seen_ = false;
	fx::Vec3 last_killer_{};
	bool turned_counted_ = false;
	DeathCameraStats stats_;
};

}
