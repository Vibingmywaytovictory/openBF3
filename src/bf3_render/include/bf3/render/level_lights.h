#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/level/spot_light.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::level {
class World;
}

namespace bf3::render {

inline constexpr size_t kMaxSceneLights = size_t(1) << 15;
inline constexpr size_t kMaxLightComponents = 4096;

enum class SceneLightSource : uint8_t {
	Placed,
	HealthState,
};

struct SceneLight {
	SceneLightSource source = SceneLightSource::Placed;
	uint32_t object = 0;
	core::level::LevelTransform transform;
	core::Vec3 color{};
	float intensity = 0.0f;
	float radius = 0.0f;
	bool specular = false;
	std::optional<core::level::SpotLight> spot;
};

struct SceneLightStats {
	size_t placed_point = 0, placed_spot = 0;
	size_t health_state_point = 0, health_state_spot = 0;
	size_t invisible = 0;
	size_t switched_off = 0;
	size_t unlit = 0;
	size_t end_of_round = 0;
	size_t unusable = 0;

	size_t found() const { return placed_point + placed_spot + health_state_point + health_state_spot; }
	size_t excluded() const { return invisible + switched_off + unlit + end_of_round + unusable; }
};

struct SceneLightPlan {
	std::vector<SceneLight> lights;
	SceneLightStats stats;
	std::vector<std::string> notes;

	size_t points() const;
	size_t spots() const;
};

core::Result<SceneLightPlan> plan_level_lights(const core::EbxResolver &p_db, const core::level::World &p_world);

void nearest_lights(
		std::span<const SceneLight> p_lights, const core::Vec3 &p_eye, size_t p_budget, std::vector<uint32_t> &r_shown);

}
