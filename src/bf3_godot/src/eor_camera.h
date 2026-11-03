#pragma once

#include "bf3/core/level/eor_camera.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/render/level_scene.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace godot {
class Camera3D;
class Node;
class Shader;
class SpotLight3D;
}

namespace bf3::godot_side {

class LevelUpload;

struct EorCameraStats {
	uint64_t taken = 0;
	uint64_t released = 0;
	uint64_t frames = 0;
};

class EorCameraView {
public:
	void frame(godot::Camera3D *p_camera, const std::optional<core::level::EorCamera> &p_camera_data,
			const core::ui::HudEndOfRound &p_end_of_round);
	bool active() const { return active_; }
	const EorCameraStats &stats() const { return stats_; }

private:
	bool active_ = false;
	godot::Transform3D player_view_;
	float player_fov_deg_ = 0.0f;
	EorCameraStats stats_;
};

struct EorShowroomStats {
	size_t backdrop_materials = 0;
	size_t cone_materials = 0;
	size_t samplers_wanted = 0;
	size_t samplers_bound = 0;
	bool light_placed = false;
	bool light_visible = false;
};

class EorShowroom {
public:
	EorShowroom();
	~EorShowroom();
	EorShowroom(const EorShowroom &) = delete;
	EorShowroom &operator=(const EorShowroom &) = delete;

	void frame(godot::Node *p_parent, const bf3::render::LevelScenePlan &p_plan, const LevelUpload &p_upload,
			const std::optional<core::level::EorCamera> &p_camera, bool p_light_on);
	const EorShowroomStats &stats() const { return stats_; }
	std::string describe() const;

private:
	struct Material {
		uint32_t index = 0;
		bool shaded = false;
		std::vector<bool> bound;
	};
	void collect(const bf3::render::LevelScenePlan &p_plan);
	void bind(const bf3::render::LevelScenePlan &p_plan, const LevelUpload &p_upload);
	void place_light(godot::Node *p_parent, const core::level::EorSpotLight &p_light);

	bool collected_ = false;
	bool complete_ = false;
	std::vector<Material> materials_;
	godot::Ref<godot::Shader> backdrop_shader_;
	godot::Ref<godot::Shader> cone_shader_;
	godot::SpotLight3D *light_ = nullptr;
	EorShowroomStats stats_;
};

}
