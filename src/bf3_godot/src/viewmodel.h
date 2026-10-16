#pragma once
// Bf3Viewmodel: the own soldier's 1p viewmodel and ADS camera (docs/design/m2a.md §11.1, §11.2; owner vm;
// docs/design/m2a-vm.md). Main thread only. build() turns a loaded bf3::render::ViewmodelSet (the loader thread's
// result) into Godot resources: one Skeleton3D for the 1p rig, the team's arms and every item's weapon mesh with its
// own Skin (each item's binds start with the arms' binds, so the arms mesh draws with whichever item is in hand),
// StandardMaterial3Ds with fov_override / z_clip_scale. select_item() puts an item in hand; present() runs one
// display frame:
//
//   ADS       anim::AdsCamera on PresentedWeapon::zoomed, refused while deploying or sprinting (anim::ads_allowed,
//             §16 Q21): world FOV 55 -> 40 over the aiming controller's FOV time, the viewmodel FOV RenderFov ->
//             ZoomRenderFov over its ZoomTransitionTime, mouse look scale LookSpeedMultiplier (look_scale()).
//   animator  anim::ViewmodelAnimator on the frame's phase, speed, view rates and the drained cues.
//   camera    the eye, the local view + the interpolated predicted recoil + the presentation kick, the world FOV
//             (fp_camera.h's M2a vm block).
//   rig       skeleton = camera * weapon_offset * Ry(pi) * inverse(model[CameraJoint]) (the camera on the rig's
//             CameraJoint, turned 180 deg about Y as in M9's demo: Frostbite looks down +Z, Godot down -Z); every
//             bone's local pose; every viewmodel material's fov_override.
//   vignette  the procedural optic vignette (§16 Q6) when the aimed zoom level is an optic (FOV <=
//             render::kOpticVignetteMaxFov), faded with the FOV progress. The default kits' iron sights do not use it.
//
// cl's integration (wave 3) fills ViewmodelFrame from host::present_weapon and the drained OwnShot / OwnTimed
// PresentEvents (viewmodel_frame_from, viewmodel_cues_from); the vm demo scene drives it from a scripted timeline.

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
} // namespace godot

namespace bf3::host {
struct PresentedWeapon;
struct PresentEvent;
} // namespace bf3::host

namespace bf3::godot_side {

// One display frame's inputs (the presented own weapon and view).
struct ViewmodelFrame {
	double time_s = 0; // the presented predicted time
	float dt = 0; // display frame time, seconds
	uint8_t phase = 0; // sim::weapons::WeaponPhase
	double phase_start_s = 0, phase_end_s = 0;
	int32_t magazine = 0;
	bool zoomed = false; // PresentedWeapon::zoomed
	bool sprinting = false;
	bool grounded = true;
	float speed = 0; // horizontal m/s
	float recoil_pitch_deg = 0, recoil_yaw_deg = 0; // the interpolated predicted recoil
	godot::Vector3 eye; // the presented eye
	double yaw = 0, pitch = 0; // the local view, sim convention, radians
	float view_yaw_rate = 0, view_pitch_rate = 0; // radians per second
};

// The frame of a PresentedWeapon (cl's present_weapon) plus the soldier's view and motion.
ViewmodelFrame viewmodel_frame_from(const bf3::host::PresentedWeapon &p_weapon, const godot::Vector3 &p_eye, double p_yaw, double p_pitch, float p_dt,
		bool p_sprinting, float p_speed, float p_view_yaw_rate, float p_view_pitch_rate);
// The cues of drained own events (OwnShot -> Shot, OwnTimed -> the weapon_events bit's cue), in time order;
// p_magazine is the presented magazine (an empty magazine at kReloadStarted plays the empty reload).
void viewmodel_cues_from(std::span<const bf3::host::PresentEvent> p_events, int32_t p_magazine, std::vector<bf3::anim::VmCue> &r_cues);

class Bf3Viewmodel : public godot::Node3D {
	GDCLASS(Bf3Viewmodel, godot::Node3D)

public:
	Bf3Viewmodel();
	~Bf3Viewmodel() override;

	// Builds the resources of p_set (which must outlive this node's use of it). Returns false (and draws nothing)
	// when the set has no items or no CameraJoint.
	bool build(const bf3::render::ViewmodelSet &p_set, const bf3::anim::ViewmodelTuning &p_tuning);
	bool built() const { return set_ != nullptr; }
	size_t item_count() const;
	// The item in hand (index into the set's items; -1 = none). Resets the animator, the ADS state and the springs.
	void select_item(int p_index);
	int item() const { return item_; }
	// Replaces the tuning; the item in hand is selected again (animator, ADS state and springs reset).
	void set_tuning(const bf3::anim::ViewmodelTuning &p_tuning);
	const bf3::anim::ViewmodelTuning &tuning() const { return tuning_; }
	const bf3::render::ViewmodelItem *current_item() const;

	// One display frame (see the header comment). p_camera is placed; the rig follows it.
	void present(const ViewmodelFrame &p_frame, std::span<const bf3::anim::VmCue> p_cues, godot::Camera3D *p_camera);

	const bf3::anim::AdsFrame &ads() const { return ads_frame_; }
	const bf3::anim::AdsParams &ads_params() const { return ads_params_; }
	bool ads_allowed() const { return ads_allowed_; }
	const bf3::anim::ViewmodelOutput &output() const { return output_; }
	const bf3::anim::LocalPose &pose() const { return pose_; }
	const std::vector<bf3::anim::Xform> &model() const { return model_; }
	float look_scale() const { return ads_frame_.look_scale; }
	// The fov_override of the item's first weapon material and of the arms' first material as set on the GPU side.
	float applied_weapon_fov() const;
	float applied_arms_fov() const;
	float vignette_strength() const { return vignette_strength_; }
	godot::Skeleton3D *skeleton() const { return skeleton_; }
	int32_t camera_joint() const { return camera_joint_; }
	const bf3::anim::ViewmodelAnimator *animator() const { return animator_.get(); }
	double last_present_us() const { return last_present_us_; }

protected:
	static void _bind_methods();

private:
	struct ItemNodes {
		godot::MeshInstance3D *weapon = nullptr;
		godot::Ref<godot::Skin> skin;
		std::vector<godot::Ref<godot::StandardMaterial3D>> materials;
		std::array<bf3::anim::VmClipRef, bf3::anim::kVmClips> refs{};
		struct Attachment {
			godot::MeshInstance3D *mesh = nullptr;
			int32_t bone = -1;
			godot::Transform3D xform; // in the bone's space
			bool hip = true, zoom = true;
		};
		std::vector<Attachment> attachments; // the default customization's rigid accessories (iron sights)
	};

	void set_viewmodel_fov(float p_fov);

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
	double last_present_us_ = 0;
};

} // namespace bf3::godot_side
