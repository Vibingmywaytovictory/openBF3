#pragma once

#include "bf3/core/ui/hud_model.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/spawn.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::sim {
class CollisionWorld;
struct MotorParams;
}

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3NativeHud;
struct NativeHudMinimapPose;

struct NativeHudMoment {
	const char *id = "";
	int32_t clip = 0;
	int32_t reserve = 0;
	int32_t grenades = 0;
	int32_t health = 0;
	int32_t tickets_us = 0;
	int32_t tickets_ru = 0;
	int32_t compass_frame = 0;
	float dispersion_px = 0.0f;
	bool crosshair = false;
	float world_x = 0.0f;
	float world_z = 0.0f;
	float minimap_metres_per_unit = 0.0f;
	bool pose_ambiguous = false;
};

std::span<const NativeHudMoment> native_hud_moments();

bf3::core::ui::HudModel native_hud_moment_model(const NativeHudMoment &p_moment, const bf3::core::ui::HudModel &p_base, float p_vfov_deg, float p_stage_height);
NativeHudMinimapPose native_hud_moment_pose(const NativeHudMoment &p_moment);
float native_hud_moment_view_yaw(const NativeHudMoment &p_moment);
std::optional<bf3::sim::Vec3> native_hud_moment_feet(const bf3::sim::CollisionWorld &p_world, const bf3::sim::MotorParams &p_motor, std::span<const bf3::sim::SpawnPoint> p_spawns,
		float p_x, float p_z);
std::optional<bf3::sim::Vec3> native_hud_moment_floor_near(const bf3::sim::CollisionWorld &p_world, const bf3::sim::MotorParams &p_motor,
		std::span<const bf3::sim::SpawnPoint> p_spawns, float p_x, float p_z, float p_radius);

class NativeHudMomentRun {
public:
	NativeHudMomentRun(std::string p_out_dir, std::string_view p_ids);

	bool step(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, double p_delta);
	int exit_code() const { return failures_ == 0 && captured_ > 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t { WaitAlive, Place, Arrive, Settle, Render, Capture, Done };
	void hold(Bf3LevelSession &r_session, const NativeHudMoment &p_moment);
	void check(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment);
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);

	std::string out_dir_;
	std::vector<const NativeHudMoment *> moments_;
	Phase phase_ = Phase::WaitAlive;
	size_t index_ = 0;
	int32_t frames_ = 0;
	double waited_ = 0.0;
	double arriving_ = 0.0;
	bf3::sim::Vec3 feet_{};
	uint32_t request_id_ = 0;
	uint32_t captured_ = 0;
	uint32_t failures_ = 0;
};

}
