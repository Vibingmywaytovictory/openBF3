#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr std::string_view kMpSoldier = "characters/soldiers/mpsoldier";

namespace soldier_sound_port {
inline constexpr int32_t kSoundMaterialId = logic_port_id("SoundMaterialId");
inline constexpr int32_t kPose = logic_port_id("Pose");
inline constexpr int32_t kIsFirstPerson = logic_port_id("IsFirstPerson");
inline constexpr int32_t kCurrentVelocityNormalized = logic_port_id("CurrentVelocityNormalized");
inline constexpr int32_t kOnStart = logic_port_id("OnStart");
inline constexpr int32_t kOnStop = logic_port_id("OnStop");
inline constexpr int32_t kOnJump = logic_port_id("OnJump");
inline constexpr int32_t kOnLand = logic_port_id("OnLand");
inline constexpr int32_t kFootPlantLeft = logic_port_id("FootPlantLeft");
inline constexpr int32_t kFootPlantRight = logic_port_id("FootPlantRight");
inline constexpr int32_t kOnEnterAir = logic_port_id("OnEnterAir");
inline constexpr int32_t kSpeed = logic_port_id("Speed");
}

struct SoldierAntStep {
	int32_t tag = 0;
	uint32_t event = 0;
};

inline constexpr size_t kMaxSoldierAntSteps = 8;

struct SoldierSoundDef {
	std::string footsteps;
	float walk_velocity_threshold = 0.0f;
	float run_velocity_threshold = 0.0f;
	float walk_period = 0.0f;
	float run_period = 0.0f;
	float start_above = 0.0f;
	float stop_at_or_below = 0.0f;
	uint32_t speed_input = 0;
	float speed_scale = 0.0f;
	std::string movement;
	std::string death;
	std::string bullet_impact;
	std::string breath_control;
	std::vector<SoldierAntStep> ant_steps;
	bool local_player_closes_period = false;
};

Result<SoldierSoundDef> read_soldier_sounds(const EbxResolver &p_db, std::string_view p_soldier = kMpSoldier);

}
