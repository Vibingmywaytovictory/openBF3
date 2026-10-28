#pragma once

#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/anim/skeleton.h"
#include "bf3/core/ant/ant_clip.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace bf3::anim {

inline constexpr float kViewmodelCrossfadeSeconds = 0.1f;

enum class VmClip : uint8_t {
	Deploy,
	Idle,
	Fire,
	FireSingle,
	Reload,
	ReloadEmpty,
	Bolt,
	ZoomIn,
	ZoomOut,
	ZoomIdle,
	Throw,
	Knife,
	Sprint,
	ThrowStart,
	ThrowHold,
	Count
};
inline constexpr size_t kVmClips = size_t(VmClip::Count);
const char *vm_clip_name(VmClip p_clip);

struct VmClipRef {
	const core::ant::DecodedClip *clip = nullptr;
	const ClipTracks *tracks = nullptr;
	float seconds = 0;
	bool stand_in = false;
};

struct SpringParams {
	float stiffness = 120.0f;
	float damping = 22.0f;
	float settle_epsilon = 1e-3f;
	float settle_seconds = 0.5f;

	bool operator==(const SpringParams &) const = default;
};
struct SpringState {
	float x = 0, v = 0;
};

void step_spring(const SpringParams &p_params, float p_target, float p_dt, SpringState &r_state);
bool spring_at_rest(const SpringParams &p_params, float p_target, const SpringState &p_state);

float spring_impulse_for_peak(const SpringParams &p_params, float p_peak);

struct ViewmodelTuning {
	SpringParams sway;
	float sway_scale = 0.02f;
	SpringParams bob;
	float bob_amplitude = 0.01f;
	float bob_frequency = 1.8f;
	SpringParams kick;
	float kick_back = 0.02f;
	float kick_pitch_deg = 1.0f;
	SpringParams camera_recoil;
	float ads_in_seconds = 0.09f;
	float ads_out_seconds = 0.15f;

	bool operator==(const ViewmodelTuning &) const = default;

	float sway_max = 0.05f;
	float bob_ads_scale = 0.3f;
	float sprint_blend_seconds = 0.2f;
	float camera_kick_pitch_deg = 0.3f;
	float kick_ads_scale = 0.5f;

	bool fov_transition_specific = true;
	float deploy_drop = 0.25f;
	float reload_drop = 0.12f;
};

core::Result<ViewmodelTuning> parse_viewmodel_tuning(std::string_view p_text);
std::string format_viewmodel_tuning(const ViewmodelTuning &p_tuning);

struct VmCue {
	enum class Kind : uint8_t {
		Shot,
		DeployStarted,
		DeployFinished,
		ReloadStarted,
		ReloadCommitted,
		ReloadFinished,
		ReloadCancelled,
		BoltStarted,
		Thrown,
		HoldStarted
	};
	Kind kind = Kind::Shot;
	double time_s = 0;
	bool empty_reload = false;
};
const char *vm_cue_name(VmCue::Kind p_kind);

struct ViewmodelFrameInput {
	double time_s = 0;
	float dt = 0;
	uint8_t phase = 0;
	double phase_start_s = 0, phase_end_s = 0;
	float ads = 0;
	float speed = 0;
	bool grounded = true;
	bool sprinting = false;
	float view_yaw_rate = 0, view_pitch_rate = 0;
	bool switch_back = false;
};

struct ViewmodelOutput {
	Xform weapon_offset;
	float camera_recoil_pitch_deg = 0, camera_recoil_yaw_deg = 0;
	uint32_t action_clip = uint32_t(VmClip::Idle);
	float action_time_s = 0;
	bool springs_at_rest = true;

	double action_start_s = 0;
	float action_weight = 0;
	bool action_procedural = false;
	uint32_t base_clip = uint32_t(VmClip::Idle);
	float sprint_weight = 0;
	uint32_t shots = 0;
	float loco_run_weight = 0, loco_sprint_weight = 0;
	float loco_run_frame = 0, loco_sprint_frame = 0;
	double release_end_s = -1;
};

struct VmLocomotion;
struct VmThrowFlow;

class ViewmodelAnimator {
public:
	ViewmodelAnimator(const Skeleton &p_rig, std::span<const VmClipRef, kVmClips> p_clips, const ViewmodelTuning &p_tuning);
	~ViewmodelAnimator();
	ViewmodelAnimator(const ViewmodelAnimator &) = delete;
	ViewmodelAnimator &operator=(const ViewmodelAnimator &) = delete;

	void step(const ViewmodelFrameInput &p_input, std::span<const VmCue> p_cues, LocalPose &r_pose, ViewmodelOutput &r_out);
	void reset();

	const ViewmodelTuning &tuning() const;

	std::array<SpringState, 7> springs() const;

	void set_locomotion(const VmLocomotion &p_locomotion);

	void set_throw_flow(const VmThrowFlow &p_flow);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

struct AdsParams {
	float hip_fov = 55.0f;
	float zoom_fov = 40.0f;
	float fov_in_seconds = 0.15f;
	float fov_out_seconds = 0.15f;
	float zoom_in_seconds = 0.09f;
	float zoom_out_seconds = 0.15f;
	float look_speed_hip = 1.0f;
	float look_speed_zoom = 0.5f;
	float render_fov = 55.0f;
	float zoom_render_fov = 0.0f;
	bool can_zoom = true;

	bool operator==(const AdsParams &) const = default;
};

AdsParams with_hip_fov(const AdsParams &p_params, float p_hip_fov_deg);

struct AdsFrame {
	bool zoomed = false;
	float fov_progress = 0;
	float blend = 0;
	float world_fov = 55.0f;
	float viewmodel_fov = 55.0f;
	float look_scale = 1.0f;
};

class AdsCamera {
public:
	void reset();

	AdsFrame step(const AdsParams &p_params, bool p_zoomed, float p_dt);
	const AdsFrame &last() const { return last_; }

private:
	float fov_progress_ = 0;
	float blend_progress_ = 0;
	AdsFrame last_;
};

float smoothstep01(float p_x);

inline constexpr uint8_t kVmPhaseDeploying = 1;
bool ads_allowed(uint8_t p_phase, bool p_sprinting);

struct ViewAngles {
	double yaw = 0, pitch = 0;
};
ViewAngles camera_view_angles(double p_view_yaw, double p_view_pitch, float p_recoil_pitch_deg, float p_recoil_yaw_deg, float p_kick_pitch_deg,
		float p_kick_yaw_deg);

}

namespace bf3::anim {

inline constexpr float kVmRunSpeedUnit = 0.06f * 60.0f;
inline constexpr float kVmMovingSpeed = 0.0015f * 60.0f;

struct VmDeltaClip {
	VmClipRef clip;
	float start_frame = 0.0f, end_frame = 0.0f;
};

struct VmLocomotion {
	VmDeltaClip run;
	VmDeltaClip sprint;
};

struct VmThrowFlow {
	float release_from_s = 0.0f, release_until_s = 0.0f, release_in_s = 0.0f;
	float hold_from_s = 0.0f, hold_loop_s = 0.0f;
	float release_end_s = 0.0f;
	bool skip_undeploy = false;
	bool valid() const;
};

}
