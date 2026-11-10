#pragma once

#include "bf3/core/gameplay/soldier_camera.h"
#include "bf3/fx/fx_types.h"

#include <cstdint>
#include <type_traits>

namespace bf3::sim {
class CollisionWorld;
}
namespace bf3::core::gameplay {
struct DeathTimeline;
}

namespace bf3::godot_side {

inline constexpr float kChaseMinUpdateRate = 10.0f;
inline constexpr float kChaseMaxAccumulated = 0.5f;
inline constexpr float kChasePillStep = 1.0f / 60.0f;
inline constexpr float kChasePillMinLength = 1.0e-4f;

struct ChaseTarget {
	fx::Vec3 eye{};
	float yaw_rad = 0.0f;
	float pitch_rad = 0.0f;
	fx::Vec3 velocity{};
};
static_assert(std::is_trivially_copyable_v<ChaseTarget>);

struct ChaseFrame {
	fx::Vec3 pivot{};
	fx::Vec3 forward{};
	fx::Vec3 right{};
	fx::Vec3 direction{};
};

ChaseFrame chase_frame(const core::gameplay::ChaseCameraDef &p_def, const ChaseTarget &p_target);

class ChaseCamera {
public:
	explicit ChaseCamera(const core::gameplay::ChaseCameraDef &p_def = {});

	void activate(const ChaseTarget &p_target, const sim::CollisionWorld *p_world);
	fx::Transform step(double p_dt, const ChaseTarget &p_target, const sim::CollisionWorld *p_world);
	const fx::Vec3 &position() const { return position_; }
	const fx::Vec3 &velocity() const { return velocity_; }
	const fx::Vec3 &wanted() const { return wanted_; }

private:
	void pill(const ChaseFrame &p_frame, double p_dt, const sim::CollisionWorld *p_world);
	void dynamics(const ChaseFrame &p_frame, const ChaseTarget &p_target);
	fx::Transform view(const ChaseFrame &p_frame) const;

	core::gameplay::ChaseCameraDef def_;
	fx::Vec3 position_{};
	fx::Vec3 previous_{};
	fx::Vec3 velocity_{};
	fx::Vec3 wanted_{};
	float length_ = 0.0f;
	float radius_ = 0.0f;
	float grow_speed_ = 0.0f;
	double accumulated_ = 0.0;
	double pill_accumulated_ = 0.0;
};

struct DeathCameraParams {
	float killer_camera_s = 1.0f;
	float fade_out_s = 0.0f;
	float fade_in_s = 0.0f;
	bool release_if_target_lost = false;
	bool has_chase = false;
	core::gameplay::ChaseCameraDef chase;
};
static_assert(std::is_trivially_copyable_v<DeathCameraParams>);

DeathCameraParams death_camera_params(const core::gameplay::DeathTimeline &p_death,
		const core::gameplay::SoldierCameras *p_cameras, const core::gameplay::PlayerCameraDef *p_killer_camera);

struct DeathCameraStart {
	uint8_t killer_slot = 0xFF;
	bool killer_camera = true;
};
static_assert(std::is_trivially_copyable_v<DeathCameraStart>);

struct DeathCameraView {
	bool active = false;
	float black = 0.0f;
	fx::Transform camera;
};
static_assert(std::is_trivially_copyable_v<DeathCameraView>);

struct DeathCameraStats {
	uint64_t views = 0;
	uint64_t killer_views = 0;
	uint64_t released_lost = 0;
	uint64_t bad_inputs = 0;
};
static_assert(std::is_trivially_copyable_v<DeathCameraStats>);

class Bf3DeathCamera {
public:
	explicit Bf3DeathCamera(const DeathCameraParams &p_params = {});

	void begin(const DeathCameraStart &p_start, double p_now_s);
	void end();
	bool active() const;
	bool on_killer() const { return taken_; }

	DeathCameraView step(double p_now_s, const ChaseTarget *p_killer, const sim::CollisionWorld *p_world);
	const DeathCameraParams &params() const { return params_; }
	DeathCameraStats stats() const { return stats_; }

private:
	float black_at(double p_now_s) const;

	DeathCameraParams params_;
	DeathCameraStart start_{};
	ChaseCamera chase_;
	fx::Transform last_;
	double start_s_ = 0.0;
	double taken_s_ = 0.0;
	double last_s_ = 0.0;
	bool active_ = false;
	bool taken_ = false;
	DeathCameraStats stats_;
};

}
