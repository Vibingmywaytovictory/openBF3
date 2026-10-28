#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/world.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace bf3::core::level {

inline constexpr std::string_view kEorCameraEvent = "ShowTopPlayers";
inline constexpr std::string_view kCameraTakeControl = "TakeControl";

struct EorCamera {
	uint32_t object = 0;
	LevelTransform transform;
	float fov_deg = 0.0f;
	int32_t priority = 0;
};

Result<std::optional<EorCamera>> read_eor_camera(const EbxResolver &p_db, const World &p_world);

}
