#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/soldier_sound.h"
#include "bf3/core/level/budget.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr std::string_view kMpGlobalPartition = "gameplay/level_setups/components/mp_global";
inline constexpr std::string_view kPlayerKillerCamera = "PlayerKillerCamera";

enum class SoldierCameraKind : uint8_t { FirstPerson, Chase, Other };
enum class CameraTargetMode : uint8_t { FirstPerson, ThirdPerson, Index };

struct ChaseCameraDef {
	Vec3 target_offset;
	float target_rotation_offset_deg = 0.0f;
	float wanted_angle_deg = 0.0f;
	float wanted_distance = 0.0f;
	float max_distance = 0.0f;
	float snap_distance = 0.0f;
	Vec3 to_wanted_position_scale;
	float velocity_drag = 0.0f;
	float max_velocity = 0.0f;
	float away_from_target_force_scale = 0.0f;
	float force_field_radius = 0.0f;
	float force_field_force_scale = 0.0f;
	float collision_radius = 0.0f;
	bool has_collision = false;
	float pill_minimum_collision_radius = 0.0f;
	float pill_maximum_collision_radius = 0.0f;
	float pill_minimum_collision_length = 0.0f;
	float pill_expand_size_speed_acceleration = 0.0f;
	float look_distance_scale = 0.0f;
	float look_distance_in_front_of_target = 0.0f;
	float max_view_rotation_angle_deg = 0.0f;
	bool keep_target_pitch = false;
	bool should_roll_with_target = false;
	bool inherit_target_velocity = false;
	float update_rate = 0.0f;
};

struct SoldierCameraDef {
	SoldierCameraKind kind = SoldierCameraKind::Other;
	std::string type;
	float shake_factor = 0.0f;
	float fade_time = 0.0f;
	ChaseCameraDef chase;
};

struct PlayerCameraDef {
	CameraTargetMode soldier_target_mode = CameraTargetMode::Index;
	int32_t soldier_camera_index = 0;
	bool release_if_target_lost = false;
	float fade_out_s = 0.0f;
	float fade_in_s = 0.0f;
};

struct SoldierCameras {
	std::vector<SoldierCameraDef> cameras;
	const SoldierCameraDef *first_person() const;
	const SoldierCameraDef *target_of(const PlayerCameraDef &p_player) const;
};

Result<ChaseCameraDef> read_chase_camera(const EbxResolver &p_db, const EbxStruct &p_chase);
Result<SoldierCameras> read_soldier_cameras(const EbxResolver &p_db, std::string_view p_soldier = kMpSoldier);
Result<PlayerCameraDef> read_player_camera(
		const EbxResolver &p_db, level::WorkBudget &r_budget, std::string_view p_name = kPlayerKillerCamera);

}
