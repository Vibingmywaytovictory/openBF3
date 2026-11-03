#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/world.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::core::level {

inline constexpr std::string_view kEorCameraEvent = "ShowTopPlayers";
inline constexpr std::string_view kCameraTakeControl = "TakeControl";

struct EorSpotLight {
	uint32_t object = 0;
	LevelTransform transform;
	Vec3 color{};
	float intensity = 0.0f;
	float radius = 0.0f;
	float attenuation_offset = 0.0f;
	float cone_inner_deg = 0.0f;
	float cone_outer_deg = 0.0f;
	bool cast_shadows = false;
	bool switched_with_environment = false;
};

struct EorEnvironment {
	uint32_t object = 0;
	std::string visual_environment;
	float visibility = 0.0f;
	bool visibility_from_proximity = false;
};

struct EorCamera {
	uint32_t object = 0;
	LevelTransform transform;
	float fov_deg = 0.0f;
	int32_t priority = 0;
	std::optional<EorSpotLight> light;
	std::optional<EorEnvironment> environment;
};

Result<std::optional<EorCamera>> read_eor_camera(const EbxResolver &p_db, const World &p_world);

}
