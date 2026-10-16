#pragma once
// L1 look (README "L1 look"): the level's visual environment (bf3_render look_environment.h) on Godot: one
// DirectionalLight3D, one WorldEnvironment with a custom sky shader, auto exposure (CameraAttributesPractical) within BF3's clamps,
// depth fog, filmic tonemap, glow and the colour-correction LUTs (ImageTexture3D), blended per frame at the camera
// over the VE areas. Main thread only (it creates and changes nodes and resources).
//
// Sky shader: the background pass draws the BF3 panorama (sky_subway_02_d on Metro) over the upper hemisphere,
// u = PanoramicRotation - atan2(x, z) / 2 pi, v = (acos(y) / pi - UVMinY) / (UVMaxY - UVMinY), blended by the
// panorama alpha texture's red channel over the SkyGradientTexture (sampled at v = acos(y) / pi), all times
// BrightnessScale [inferred: the mapping of the panorama and gradient]. The cubemap pass (Godot's radiance and
// irradiance, i.e. the ambient and the reflections) draws the three-band ambient instead (LookFrame ambient_up /
// horizon / down), so the ambient keeps Enlighten's level rather than the panorama's (5x brighter in the city).
// Sky parameters are only re-sent when a value moved by more than 1 % (a change re-renders the radiance map).

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
} // namespace godot

namespace bf3::godot_side {

class LevelUpload;

class LookBridge {
public:
	LookBridge();
	~LookBridge();
	LookBridge(const LookBridge &) = delete;
	LookBridge &operator=(const LookBridge &) = delete;

	// Creates the sun and the environment under p_parent from p_plan (an L1 plan). Main thread.
	void build(godot::Node *p_parent, std::shared_ptr<const bf3::render::LevelScenePlan> p_plan);
	bool built() const { return plan_ != nullptr; }
	// Every frame: binds sky textures that have arrived and applies the blend at p_camera (Frostbite = Godot space).
	void update(const LevelUpload &p_upload, const godot::Vector3 &p_camera);
	// One line: the last applied frame.
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
	std::array<bool, 3> sky_bound_{}; // panorama, alpha, gradient
	std::optional<bf3::render::LookFrame> applied_; // the last frame (describe())
	std::optional<bf3::render::LookFrame> sent_; // per group, the values last sent to the RenderingServer
};

} // namespace bf3::godot_side
