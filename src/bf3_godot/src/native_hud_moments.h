#pragma once

#include "native_hud.h"

#include "bf3/core/ui/hud_model.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/spawn.h"

#include <array>
#include <cstdint>
#include <map>
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

struct NativeHudSquadRow {
	int16_t kit = 0;
	int16_t booster = 0;
	bool leader = false;
};

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
	float map_texel_x = 0.0f;
	float map_texel_y = 0.0f;
	float map_rotation_deg = 0.0f;
	float area_texel_x = 0.0f;
	float area_texel_y = 0.0f;
	float area_rotation_deg = 0.0f;
	int32_t compass_next = 0;
	bool detail_ambiguous = false;
};

std::span<const NativeHudMoment> native_hud_moments();

bf3::core::ui::HudModel native_hud_moment_model(const NativeHudMoment &p_moment, const bf3::core::ui::HudModel &p_base, double p_crosshair_base_offset);
float native_hud_moment_heading_deg(const NativeHudMoment &p_moment);
float native_hud_moment_view_yaw(const NativeHudMoment &p_moment);
std::array<float, 2> native_hud_moment_texel(const NativeHudMoment &p_moment);
float native_hud_moment_registered_heading_deg(const NativeHudMoment &p_moment);
NativeHudMinimapPose native_hud_moment_pose(const bf3::sim::Vec3 &p_own, float p_view_yaw);
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
	enum class Phase : uint8_t { WaitAlive, Place, Arrive, Settle, Render, Capture, Coverage, Done };
	void hold(Bf3LevelSession &r_session, const NativeHudMoment &p_moment);
	void settle(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment, bf3::core::ui::HudModel p_model);
	void check(Bf3LevelSession &r_session, Bf3NativeHud &r_hud, const NativeHudMoment &p_moment);
	void check_messages(Bf3NativeHud &r_hud, const NativeHudMoment &p_moment);
	void check_squad(Bf3NativeHud &r_hud, const NativeHudMoment &p_moment);
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
	std::vector<std::string> expect_kills_;
	std::vector<int32_t> expect_kill_frames_;
	std::vector<float> reference_kill_alphas_;
	std::vector<std::string> expect_log_;
	int32_t expect_score_frame_ = -1;
	int32_t expect_aggregator_frame_ = -1;
	float name_scale_ = 0.0f;
	std::string names_detail_;
	godot::Ref<godot::Image> composite_;
	std::array<godot::Ref<godot::Image>, kNativeHudViews> colour_;
	std::vector<NativeHudSquadRow> expect_squad_;
	std::vector<std::string> expect_squad_names_;
	std::map<std::string, godot::PackedByteArray, std::less<>> first_capture_;
};

}
