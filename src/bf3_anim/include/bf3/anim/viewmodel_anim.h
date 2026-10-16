#pragma once
// The 1p viewmodel animator and its springs (docs/design/m2a.md §11.1, §11.2; owner vm). Godot-free (bf3_anim links
// bf3_core only, so the inputs are plain numbers: phases are sim::weapons::WeaponPhase values, times are seconds of
// the predicted timeline). Bf3Viewmodel fills ViewmodelFrameInput from bf3_host's PresentedWeapon and the drained
// OwnTimed / OwnShot PresentEvents once per display frame and draws the pose.
//
// Layers: base (idle, sprint), action (deploy, fire, reload, bolt, throw, knife) with kViewmodelCrossfadeSeconds
// crossfades, ADS blend. One evaluation per frame (~20 us [inferred]).
// Reload timing (E5; §16 Q4, taken 2026-10-16: authored rate): the reload clip starts at the predicted
// kReloadStarted time and plays at its authored rate; the weapon is ready at ReloadTime from the sim, and a shot after
// that cuts the clip with a kViewmodelCrossfadeSeconds blend (M9's empty reload clip is 2.167 s long against its 1.60 s
// ReloadTime, so clip end != ready). Weapon switch: the new item's deploy plays at kDeployStarted.
// Springs (presentation only, viewmodel_tuning.cfg): sway (view-rate lag), bob (move speed) and the recoil kick (BF3
// CameraRecoilData spring on the camera plus a weapon-local kick). IW4L's weapon_iw4 view_bob, sway and kick were
// studied for the spring forms; the constants are tuned by the user's feel (§17). Each spring comes to rest within
// its settle time once its input stops (E19).

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

inline constexpr float kViewmodelCrossfadeSeconds = 0.1f; // action layer crossfades and the fire-cuts-reload blend

// The clip roles (= render::ViewmodelClipRole order; bf3_render includes this library, not the other way round).
enum class VmClip : uint8_t { Deploy, Idle, Fire, FireSingle, Reload, ReloadEmpty, Bolt, ZoomIn, ZoomOut, ZoomIdle, Throw, Knife, Count };
inline constexpr size_t kVmClips = size_t(VmClip::Count);

// One clip on the 1p rig, or a procedural stand-in (clip == nullptr: the role's procedural motion over `seconds`).
struct VmClipRef {
	const core::ant::DecodedClip *clip = nullptr;
	const ClipTracks *tracks = nullptr;
	float seconds = 0;
	bool stand_in = false;
};

// One spring (critically damped by default): x'' = -stiffness (x - target) - damping x'.
struct SpringParams {
	float stiffness = 120.0f;
	float damping = 22.0f;
	float settle_epsilon = 1e-3f; // |x - target| and |x'| below this count as at rest
	float settle_seconds = 0.5f; // E19: at rest within this once the input stops

	bool operator==(const SpringParams &) const = default;
};
struct SpringState {
	float x = 0, v = 0;
};
// One explicit, sub-stepped update of p_dt seconds (stable for any frame time up to 0.25 s).
void step_spring(const SpringParams &p_params, float p_target, float p_dt, SpringState &r_state);
bool spring_at_rest(const SpringParams &p_params, float p_target, const SpringState &p_state);

struct ViewmodelTuning {
	SpringParams sway; // view-rate lag (yaw and pitch)
	float sway_scale = 0.02f; // metres of lag per radian per second of view rate
	SpringParams bob; // move-speed bob
	float bob_amplitude = 0.01f; // metres at 4 m/s
	float bob_frequency = 1.8f; // cycles per second at 4 m/s
	SpringParams kick; // weapon-local kick per shot
	float kick_back = 0.02f; // metres
	float kick_pitch_deg = 1.0f;
	SpringParams camera_recoil; // CameraRecoilData spring on the camera
	float ads_in_seconds = 0.09f; // iron sights zoom-in (aiming controller, topics/01 §1.4) [verified data]
	float ads_out_seconds = 0.15f;

	bool operator==(const ViewmodelTuning &) const = default;
};
// viewmodel_tuning.cfg ("key = value", motor_tuning.cfg's format; keys are the field paths, e.g. sway.stiffness).
core::Result<ViewmodelTuning> parse_viewmodel_tuning(std::string_view p_text);
std::string format_viewmodel_tuning(const ViewmodelTuning &p_tuning);

// A one-shot cue of the frame (from the drained OwnShot / OwnTimed events), in time order.
struct VmCue {
	enum class Kind : uint8_t { Shot, DeployStarted, DeployFinished, ReloadStarted, ReloadCommitted, ReloadFinished, ReloadCancelled, BoltStarted, Thrown };
	Kind kind = Kind::Shot;
	double time_s = 0; // predicted timeline
	bool empty_reload = false; // ReloadStarted with an empty magazine (ReloadEmpty clip)
};

struct ViewmodelFrameInput {
	double time_s = 0; // presented predicted time
	float dt = 0; // display frame time, seconds
	uint8_t phase = 0; // sim::weapons::WeaponPhase of the active slot
	double phase_start_s = 0, phase_end_s = 0;
	float ads = 0; // 0 hip .. 1 aimed (the presented zoom level)
	float speed = 0; // horizontal m/s
	bool grounded = true;
	bool sprinting = false;
	float view_yaw_rate = 0, view_pitch_rate = 0; // radians per second (sway)
};

struct ViewmodelOutput {
	Xform weapon_offset; // springs: sway + bob + kick, in camera space
	float camera_recoil_pitch_deg = 0, camera_recoil_yaw_deg = 0; // the presentation kick the camera adds
	uint32_t action_clip = uint32_t(VmClip::Idle); // VmClip of the action layer (reports)
	float action_time_s = 0;
	bool springs_at_rest = true;
};

class ViewmodelAnimator {
public:
	ViewmodelAnimator(const Skeleton &p_rig, std::span<const VmClipRef, kVmClips> p_clips, const ViewmodelTuning &p_tuning);
	~ViewmodelAnimator();
	ViewmodelAnimator(const ViewmodelAnimator &) = delete;
	ViewmodelAnimator &operator=(const ViewmodelAnimator &) = delete;

	// One display frame: cues first (in order), then the layers at p_input.time_s, then the springs over p_input.dt.
	void step(const ViewmodelFrameInput &p_input, std::span<const VmCue> p_cues, LocalPose &r_pose, ViewmodelOutput &r_out);
	void reset(); // a new life or a kit change

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::anim
