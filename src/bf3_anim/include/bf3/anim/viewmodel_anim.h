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
//
// As built (vm, docs/design/m2a-vm.md):
//   * Base layer: the item's IdlePose frame; ADS blends to its ZoomPose frame. While the ADS blend moves, a decoded
//     zoomInOut / ZoomOut clip (DCT on the rifles) is sampled at the blend's normalised progress, so its authored
//     path is played over the aiming controller's ZoomTransitionTime; a VBR zoom clip (pistols, not decoded) or a
//     reversal mid-transition uses the IdlePose -> ZoomPose blend (the VBR stand-in of viewmodel_set.h). Sprint blends
//     to SprintPose (or a procedural lowered offset without one) over ViewmodelTuning::sprint_blend_seconds.
//   * Action layer: one running action and the one it replaced (crossfading out over kViewmodelCrossfadeSeconds).
//     A clip action fades out over its last kViewmodelCrossfadeSeconds; a deploy cuts in (the previous item is a
//     different mesh). The hip Fire clip is weighted by (1 - ADS): an aimed shot is the procedural kick (BF3's
//     zoomFire is VBR on the pistols and absent on the rifles).
//   * Procedural stand-ins (no clip, VmClipRef::clip == nullptr with seconds > 0): deploy raises the weapon from
//     below over `seconds` (DeployTime), reload lowers and tilts it over `seconds`, throw and knife swing it, fire is
//     the kick spring only.
//   * Camera-space convention of ViewmodelOutput::weapon_offset: Godot's camera frame (x right, y up, z towards the
//     eye; the camera looks down -z). Bf3Viewmodel applies it between the camera and the rig's CameraJoint.

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
// Sprint (vm, additive): the SprintPose frame of the item's package (the base layer's sprint).
enum class VmClip : uint8_t { Deploy, Idle, Fire, FireSingle, Reload, ReloadEmpty, Bolt, ZoomIn, ZoomOut, ZoomIdle, Throw, Knife, Sprint, Count };
inline constexpr size_t kVmClips = size_t(VmClip::Count);
const char *vm_clip_name(VmClip p_clip);

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
// One update of p_dt seconds (clamped to 0.25 s) with the target held. vm: the closed-form solution, so the state
// after a second is the same at every frame rate and stable for any stiffness and damping.
void step_spring(const SpringParams &p_params, float p_target, float p_dt, SpringState &r_state);
bool spring_at_rest(const SpringParams &p_params, float p_target, const SpringState &p_state);
// The velocity impulse that makes a spring at rest peak at p_peak (critically damped: v0 = peak * omega * e; other
// damping ratios: the same formula, an approximation).
float spring_impulse_for_peak(const SpringParams &p_params, float p_peak);

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

	// ---- vm additions (viewmodel_tuning.cfg) ----
	float sway_max = 0.05f; // metres: the sway lag target is clamped to +- this
	float bob_ads_scale = 0.3f; // bob amplitude while fully aimed
	float sprint_blend_seconds = 0.2f; // base layer idle <-> sprint
	float camera_kick_pitch_deg = 0.3f; // the camera spring's peak per shot (presentation kick, on top of the predicted recoil)
	float kick_ads_scale = 0.5f; // weapon-local kick while fully aimed
	// ADS FOV time: true = the aiming controller's ZoomLevelSpecificTransitionTime of the transition (iron sights:
	// in 0.12 s, out 0.15 s [verified data]); false = its generic FovTransitionTime (0.15 s) both ways.
	bool fov_transition_specific = true;
	float deploy_drop = 0.25f; // metres below the eye at the start of a procedural deploy
	float reload_drop = 0.12f; // metres the procedural reload lowers the weapon
};
// viewmodel_tuning.cfg ("key = value", motor_tuning.cfg's format; keys are the field paths, e.g. sway.stiffness).
// Unknown, repeated, malformed or out-of-range keys fail with the line number; keys left out keep the defaults.
// format_viewmodel_tuning writes every key with round-trip number text, so parse(format(t)) == t.
core::Result<ViewmodelTuning> parse_viewmodel_tuning(std::string_view p_text);
std::string format_viewmodel_tuning(const ViewmodelTuning &p_tuning);

// A one-shot cue of the frame (from the drained OwnShot / OwnTimed events), in time order.
struct VmCue {
	enum class Kind : uint8_t { Shot, DeployStarted, DeployFinished, ReloadStarted, ReloadCommitted, ReloadFinished, ReloadCancelled, BoltStarted, Thrown };
	Kind kind = Kind::Shot;
	double time_s = 0; // predicted timeline
	bool empty_reload = false; // ReloadStarted with an empty magazine (ReloadEmpty clip)
};
const char *vm_cue_name(VmCue::Kind p_kind);

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

	// ---- vm additions (reports and checks) ----
	double action_start_s = 0; // predicted time the running action started at (its cue's time)
	float action_weight = 0; // weight of the running action over the base layer
	bool action_procedural = false; // the running action is a procedural stand-in
	uint32_t base_clip = uint32_t(VmClip::Idle); // Idle, ZoomIdle, ZoomIn, ZoomOut or Sprint: what drives the base layer
	float sprint_weight = 0;
	uint32_t shots = 0; // Shot cues handled since reset()
	float pt1_run_weight = 0, pt1_sprint_weight = 0; // PT1 run [api]: the 1p run / sprint deltas' weights (0 without them)
	float pt1_run_frame = 0, pt1_sprint_frame = 0; // PT1 run [api]: their clip frames
};

struct VmLocomotion; // PT1 run [api]: BF3's 1p run and sprint deltas (the PT1 run block at the end of this file)

class ViewmodelAnimator {
public:
	ViewmodelAnimator(const Skeleton &p_rig, std::span<const VmClipRef, kVmClips> p_clips, const ViewmodelTuning &p_tuning);
	~ViewmodelAnimator();
	ViewmodelAnimator(const ViewmodelAnimator &) = delete;
	ViewmodelAnimator &operator=(const ViewmodelAnimator &) = delete;

	// One display frame: cues first (in order), then the layers at p_input.time_s, then the springs over p_input.dt.
	void step(const ViewmodelFrameInput &p_input, std::span<const VmCue> p_cues, LocalPose &r_pose, ViewmodelOutput &r_out);
	void reset(); // a new life or a kit change

	// ---- vm additions ----
	const ViewmodelTuning &tuning() const;
	// The spring states (sway x, sway y, bob amplitude, kick back, kick pitch, camera pitch, camera yaw) for checks.
	std::array<SpringState, 7> springs() const;
	// PT1 run [api]: the item's 1p run / sprint deltas (the clips must outlive the animator); an empty set restores the bob.
	void set_locomotion(const VmLocomotion &p_locomotion); // PT1 run [api]

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// ---- ADS camera and FOV (docs/design/m2a.md §11.2; vm) --------------------------------------------------------------
//
// World FOV: ZoomLevelData.fieldOfView, hip 55 and iron sights 40 [verified data], vertical (Godot KEEP_HEIGHT) like
// RenderFov (§16 Q5). It moves linearly in time over the transition's FOV time (eased with smoothstep), so it reaches
// the target exactly when the progress reaches 1: fov_in_seconds after the presented ADS level turns on (± the frame
// the press lands in) and fov_out_seconds after it turns off. The viewmodel's ADS blend runs over the aiming
// controller's ZoomTransitionTime (iron sights: in 0.09 s, out 0.15 s [verified data]). Viewmodel FOV: RenderFov at
// hip, blended towards the weapon's ZoomRenderFov by the ADS blend when it has one. Mouse look scales by
// LookSpeedMultiplier (iron sights 0.5) along the FOV progress.
struct AdsParams {
	float hip_fov = 55.0f; // ZoomLevelData.FieldOfView of zoom level 0 (defaultbase)
	float zoom_fov = 40.0f; // of zoom level 1 (defaultironsights)
	float fov_in_seconds = 0.15f; // FOV transition 0 -> 1
	float fov_out_seconds = 0.15f; // 1 -> 0
	float zoom_in_seconds = 0.09f; // the viewmodel's ADS blend 0 -> 1 (ZoomTransitionTime)
	float zoom_out_seconds = 0.15f;
	float look_speed_hip = 1.0f; // ZoomLevelData.LookSpeedMultiplier
	float look_speed_zoom = 0.5f;
	float render_fov = 55.0f; // SoldierWeaponData.RenderFov
	float zoom_render_fov = 0.0f; // ZoomRenderFov (0 = keep RenderFov when aimed)
	bool can_zoom = true; // false: the aiming controller has one zoom level (knife, M67)

	bool operator==(const AdsParams &) const = default;
};

struct AdsFrame {
	bool zoomed = false; // the presented ADS level this frame
	float fov_progress = 0; // 0 hip .. 1 aimed, linear in time
	float blend = 0; // the viewmodel's ADS blend, eased (ViewmodelFrameInput::ads)
	float world_fov = 55.0f; // vertical degrees
	float viewmodel_fov = 55.0f; // vertical degrees (BaseMaterial3D fov_override)
	float look_scale = 1.0f;
};

class AdsCamera {
public:
	void reset();
	// One display frame of p_dt seconds with the presented ADS level p_zoomed (PresentedWeapon::zoomed).
	AdsFrame step(const AdsParams &p_params, bool p_zoomed, float p_dt);
	const AdsFrame &last() const { return last_; }

private:
	float fov_progress_ = 0;
	float blend_progress_ = 0;
	AdsFrame last_;
};

float smoothstep01(float p_x);

// §5.2 / §16 Q21: zoom is refused for the whole tick while the active weapon deploys or the soldier sprints. sv's
// step_weapons_m2a refuses it in the sim (kZoomWasDown stays clear); the presentation applies the same rule to the
// presented state, so the camera never zooms on a refused tick (also while weapon_rules.cpp is still the W2.0
// forwarding stub). p_phase is a sim::weapons::WeaponPhase value.
inline constexpr uint8_t kVmPhaseDeploying = 1; // WeaponPhase::Deploying (static_assert in viewmodel.cpp)
bool ads_allowed(uint8_t p_phase, bool p_sprinting);

// The camera's view angles: the local view plus the interpolated predicted recoil (sim::weapons::RecoilOffset:
// + pitch up, + yaw left, degrees) plus the presentation kick (ViewmodelOutput::camera_recoil_*), pitch clamped to
// +-89.99 degrees as aim_direction does. Radians in and out, sim convention (yaw 0 looks down +Z, + = left).
struct ViewAngles {
	double yaw = 0, pitch = 0;
};
ViewAngles camera_view_angles(double p_view_yaw, double p_view_pitch, float p_recoil_pitch_deg, float p_recoil_yaw_deg, float p_kick_pitch_deg,
		float p_kick_yaw_deg);

} // namespace bf3::anim

// ---- PT1 run (docs/design/pt1-run.md) --------------------------------------------------------------------------------
// BF3's 1p locomotion (s_basicassets 1P_MovementStateFlow) [verified data 2026-10-19, antdump objects]:
//   * running (RunNode): 1P_RunLoopChooser[WeaponType] -> e.g. 1P_RifleNoiseLayer -> Rifle_RunFwdLoopSeq (Looping): the
//     additive Rifle_RunFwdDelta (110 frames, 1P_Upperbody_DOF) over its sequence window StartInTime 28 .. EndInTime 180
//     ticks (frames 14 .. 90), under the SpeedScaleControllerAsset 1P_MovementSpeed_SpeedController whose SpeedValue
//     1P_RunSpeed_ESIG is OutMovementSpeed / 0.06 m per tick: the rate is speed / kVmRunSpeedUnit;
//   * sprinting (SprintNode): Wep_SprintDeltaChooser[WeaponType] -> Rifle_SprintDeltaSeq (Looping): Rifle_SprintDelta
//     (72 frames, the whole clip) at rate 1;
//   * moving: OutMovementSpeed_EBOOL, speed > 0.0015 m per tick (kVmMovingSpeed).
// The animator applies them as additive layers after the action layer (LocalPost, the crowd aim's order [inferred]):
// run at moving (ramped over sprint_blend_seconds) x (1 - sprint weight) x (1 - ADS) [named default, measured = false:
// BF3's zoomed moves are other nodes], sprint at the base layer's sprint weight. With either delta the procedural bob
// is off. The camera stays on the camera: Bf3Viewmodel holds the rig's CameraJoint at the camera, so the deltas show
// as the arms' and weapon's motion relative to the view [named default, measured = false].

namespace bf3::anim {

inline constexpr float kVmRunSpeedUnit = 0.06f * 60.0f; // m/s at rate 1 (1P_RunSpeed_ESIG = OutMovementSpeed / 0.06)
inline constexpr float kVmMovingSpeed = 0.0015f * 60.0f; // m/s (OutMovementSpeed_EBOOL's threshold, 0.0015 m/tick)

struct VmDeltaClip {
	VmClipRef clip; // an additive clip on the 1p rig (clip and tracks null: none)
	float start_frame = 0.0f, end_frame = 0.0f; // the loop window (clip frames)
};

struct VmLocomotion {
	VmDeltaClip run; // played at speed / kVmRunSpeedUnit
	VmDeltaClip sprint; // played at rate 1
};

} // namespace bf3::anim
