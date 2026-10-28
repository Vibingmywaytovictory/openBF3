#pragma once

#include "bf3/anim/viewmodel_anim.h"
#include "bf3/render/viewmodel_set.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace godot {
class Camera3D;
class CanvasLayer;
class ColorRect;
class MeshInstance3D;
class Skeleton3D;
class ShaderMaterial;
}

namespace bf3::host {
struct PresentedWeapon;
}

namespace bf3::present {
struct PresentEvent;
}

namespace bf3::godot_side {

struct ViewmodelFrame {
	double time_s = 0;
	float dt = 0;
	uint8_t phase = 0;
	double phase_start_s = 0, phase_end_s = 0;
	int32_t magazine = 0;
	bool zoomed = false;
	bool sprinting = false;
	bool grounded = true;
	float speed = 0;
	float recoil_pitch_deg = 0, recoil_yaw_deg = 0;
	godot::Vector3 eye;
	double yaw = 0, pitch = 0;
	float view_yaw_rate = 0, view_pitch_rate = 0;
	bool switch_back = false;
};

ViewmodelFrame viewmodel_frame_from(const bf3::host::PresentedWeapon &p_weapon, const godot::Vector3 &p_eye, double p_yaw, double p_pitch, float p_dt,
		bool p_sprinting, float p_speed, float p_view_yaw_rate, float p_view_pitch_rate);

void viewmodel_cues_from(std::span<const bf3::present::PresentEvent> p_events, int32_t p_magazine, std::vector<bf3::anim::VmCue> &r_cues);

class Bf3Viewmodel : public godot::Node3D {
	GDCLASS(Bf3Viewmodel, godot::Node3D)

public:
	Bf3Viewmodel();
	~Bf3Viewmodel() override;

	bool build(const bf3::render::ViewmodelSet &p_set, const bf3::anim::ViewmodelTuning &p_tuning);
	int add_items(const bf3::render::ViewmodelSet &p_set);
	bool built() const { return set_ != nullptr; }
	size_t item_count() const;

	void select_item(int p_index);
	int item() const { return item_; }

	void set_tuning(const bf3::anim::ViewmodelTuning &p_tuning);
	const bf3::anim::ViewmodelTuning &tuning() const { return tuning_; }
	const bf3::render::ViewmodelItem *current_item() const;

	void present(const ViewmodelFrame &p_frame, std::span<const bf3::anim::VmCue> p_cues, godot::Camera3D *p_camera);

	const bf3::anim::AdsFrame &ads() const { return ads_frame_; }
	const bf3::anim::AdsParams &ads_params() const { return ads_params_; }
	bool ads_allowed() const { return ads_allowed_; }
	const bf3::anim::ViewmodelOutput &output() const { return output_; }
	const bf3::anim::LocalPose &pose() const { return pose_; }
	const std::vector<bf3::anim::Xform> &model() const { return model_; }
	float look_scale() const { return ads_frame_.look_scale; }
	void set_hip_fov(float p_hip_fov_deg) { hip_fov_deg_ = p_hip_fov_deg; }
	float world_fov() const { return ads_frame_.world_fov; }

	float applied_weapon_fov() const;
	float applied_arms_fov() const;
	float vignette_strength() const { return vignette_strength_; }
	godot::Skeleton3D *skeleton() const { return skeleton_; }
	int32_t camera_joint() const { return camera_joint_; }
	const bf3::anim::ViewmodelAnimator *animator() const { return animator_.get(); }
	double last_present_us() const { return last_present_us_; }

	size_t alpha_scissor_materials(int p_item) const;

protected:
	static void _bind_methods();

private:
	struct ItemNodes {
		const bf3::render::ViewmodelItem *item = nullptr;
		godot::MeshInstance3D *weapon = nullptr;
		godot::Ref<godot::Skin> skin;
		std::vector<godot::Ref<godot::StandardMaterial3D>> materials;
		std::array<bf3::anim::VmClipRef, bf3::anim::kVmClips> refs{};
		struct Attachment {
			godot::MeshInstance3D *mesh = nullptr;
			int32_t bone = -1;
			godot::Transform3D xform;
			bool hip = true, zoom = true;
		};
		std::vector<Attachment> attachments;
	};

	void set_viewmodel_fov(float p_fov);
	void add_item_nodes(const bf3::render::ViewmodelSet &p_set);

	const bf3::render::ViewmodelSet *set_ = nullptr;
	bf3::anim::ViewmodelTuning tuning_;
	godot::Skeleton3D *skeleton_ = nullptr;
	godot::MeshInstance3D *arms_ = nullptr;
	std::vector<godot::Ref<godot::StandardMaterial3D>> arms_materials_;
	std::vector<ItemNodes> items_;
	int item_ = -1;
	int32_t camera_joint_ = -1;
	std::unique_ptr<bf3::anim::ViewmodelAnimator> animator_;
	bf3::anim::AdsCamera ads_;
	bf3::anim::AdsParams ads_params_;
	bf3::anim::AdsFrame ads_frame_;
	bool ads_allowed_ = true;
	bf3::anim::ViewmodelOutput output_;
	bf3::anim::LocalPose pose_;
	std::vector<bf3::anim::Xform> model_;
	float viewmodel_fov_ = -1;
	godot::CanvasLayer *vignette_layer_ = nullptr;
	godot::ColorRect *vignette_ = nullptr;
	godot::Ref<godot::ShaderMaterial> vignette_material_;
	float vignette_strength_ = 0;
	float hip_fov_deg_ = 0.0f;
	double last_present_us_ = 0;
};

}
