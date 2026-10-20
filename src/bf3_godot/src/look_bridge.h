#pragma once

#include "bf3/render/level_scene.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace godot {
class CameraAttributesPractical;
class DirectionalLight3D;
class Environment;
class ImageTexture3D;
class Node;
class ShaderMaterial;
class WorldEnvironment;
}

namespace bf3::godot_side {

class LevelUpload;

class LookBridge {
public:
	LookBridge();
	~LookBridge();
	LookBridge(const LookBridge &) = delete;
	LookBridge &operator=(const LookBridge &) = delete;

	void build(godot::Node *p_parent, std::shared_ptr<const bf3::render::LevelScenePlan> p_plan);
	bool built() const { return plan_ != nullptr; }

	void update(const LevelUpload &p_upload, const godot::Vector3 &p_camera);

	std::string describe() const;

private:
	void apply(const bf3::render::LookFrame &p_frame, bool p_force);

	std::shared_ptr<const bf3::render::LevelScenePlan> plan_;
	godot::DirectionalLight3D *sun_ = nullptr;
	godot::WorldEnvironment *world_ = nullptr;
	godot::Ref<godot::Environment> environment_;
	godot::Ref<godot::ShaderMaterial> sky_material_;
	godot::Ref<godot::CameraAttributesPractical> attributes_;
	std::vector<godot::Ref<godot::ImageTexture3D>> luts_;
	std::array<bool, 3> sky_bound_{};
	std::optional<bf3::render::LookFrame> applied_;
	std::optional<bf3::render::LookFrame> sent_;
};

}
