#pragma once

#include "bf3/render/level_lights.h"

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::godot_side {

inline constexpr float kLevelLightAttenuation = 2.0f;
inline constexpr float kLevelLightReselectDistance = 0.5f;
inline constexpr size_t kLevelLightReserve = 8;

size_t level_light_budget(int64_t p_clustered_elements);

class LevelLights {
public:
	LevelLights() = default;
	~LevelLights();
	LevelLights(const LevelLights &) = delete;
	LevelLights &operator=(const LevelLights &) = delete;

	void build(const bf3::render::SceneLightPlan &p_plan, const godot::RID &p_scenario);
	void update(const godot::Vector3 &p_camera);
	void set_shadow_quality(uint8_t p_quality);
	void free_all();

	std::string describe() const;

private:
	struct Light {
		godot::RID light;
		godot::RID instance;
		bool shown = false;
	};

	std::vector<bf3::render::SceneLight> plan_;
	bf3::render::SceneLightStats stats_;
	std::vector<Light> lights_;
	std::vector<uint32_t> shown_;
	godot::Vector3 selected_at_;
	bool selected_ = false;
	size_t budget_ = 0;
	size_t points_ = 0, spots_ = 0, shadowed_ = 0;
	uint8_t shadow_quality_ = 0;
	bool shadows_applied_ = false;
};

}
