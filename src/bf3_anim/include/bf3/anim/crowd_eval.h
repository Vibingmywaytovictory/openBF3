#pragma once
// Pose evaluation of the 3p crowd: per soldier, the run blend, a masked upper-body reload, the additive aim
// and the skin palette, written on worker lanes and published once per frame (S3; docs/design/s3.md §3, §5).
//
// Frame protocol (one owner thread, the Godot main thread in S3; docs/design/s3.md §3):
//   evaluator.wait_and_publish(group)       // job of the previous frame done: its palettes become front
//   ... upload the front palettes ...       // main thread, Godot
//   evaluator.set_inputs(inputs, dt)        // this frame's sim inputs
//   evaluator.kick(group)                   // evaluate every due soldier into its back palette
// So the pose a frame shows was evaluated from the inputs of the frame before it (one frame of pose latency).
// The root transform is not part of the palette: the main thread sets it from the current input, so the
// soldier's position is never late.
//
// Buffers are double-buffered PER SOLDIER: each soldier owns two palette slots and two model-pose slots and a
// front index. wait_and_publish flips the front index only for the soldiers the job evaluated, so a soldier
// skipped by temporal LOD keeps its current front palette and model pose (never one from two evaluations ago),
// and palette(i), model(i) and changed(i) always describe the pose on the GPU.
//
// Per soldier, in order (CrowdClip and LocoClip names):
//   1. loco = blend of up to two direction cycles + idle at one phase (locomotion_weights); clips of weight 0 are
//      not sampled (CrowdFrameStats counts what was)
//   2. the root joint's translation reset to rest (the sim owns the root)
//   3. weapon layout: the rig joints in CrowdTeam::weapon_joints take the weaponske01 LocalPose of the same name
//      (CrowdTeam::weapon_local) before the layers. They are the 21 Wep_* joints below Wep_Root that the idle,
//      run and aim clips leave undriven [verified: those clips drive Wep_Root and IK_Joint_Left/RightHand, the
//      reload drives all 24]. The rig's defaults for them differ from the weapon mesh's bind by up to 0.34 m
//      (Wep_Mag), so without this step the magazine would float; the reload override then moves them as authored.
//   4. reload: override_masked(loco, reload sample, reload mask, w), w ramps 0.15 s in and out
//   5. aim: apply_additive(loco, AimUp or AimDown at |pitch| / 60 deg, aim mask, order)
//   6. model_pose -> skin_matrices(binds of the soldier's team) -> back palette and back model pose
// Temporal LOD (temporal_lod.h) skips steps 1-6 for a soldier whose interval has not elapsed. Its front is
// then not flipped and not marked changed, and the uploader skips it. Updates are staggered by slot, so the
// due soldiers of a tier spread over frames.
//
// Lanes: S3 runs the job on a core::WorkerGroup of clamp(hardware_concurrency - 4, 2, 6) lanes (a
// hardware_concurrency of 0 counts as 2 lanes; s3.md §3).

#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/locomotion.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/anim/skinning.h"
#include "bf3/anim/temporal_lod.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::core {
class WorkerGroup;
}

namespace bf3::anim {

constexpr uint32_t kMaxCrowdSoldiers = 256; // hard cap (S3 runs 64)
constexpr uint32_t kMaxCrowdTeams = 2;
constexpr uint32_t kMaxPaletteBinds = 256; // one RS skeleton bone per bind (measured: US 113, RU 107)

namespace crowd_flags {
constexpr uint8_t kReloadStart = 1 << 0; // start the team's reload this frame (ignored while one is playing)
constexpr uint8_t kHidden = 1 << 1; // not drawn: no evaluation
} // namespace crowd_flags

// What the simulation (or the S3 stage) says about one soldier this frame. Plain data, copied per frame.
struct CrowdSoldierInput {
	Vec3 position{}; // feet, world space
	Vec3 velocity{}; // world space, m/s
	float yaw = 0.0f; // radians, sim convention (command.h: yaw 0 looks down +Z)
	float pitch = 0.0f; // radians, > 0 looks up
	float camera_distance = 0.0f; // metres, for temporal LOD
	uint8_t team = 0; // 0 US, 1 RU
	uint8_t flags = 0; // crowd_flags
};

struct CrowdConfig {
	bool temporal_lod = true; // false: every soldier every frame (the G1 gate case, s3.md §1)
	TemporalLodTable lod; // read_game_animation_settings, or the defaults
	LocoParams loco;
	AdditiveOrder aim_order = AdditiveOrder::LocalPost; // s3.md §16 Q2
	float aim_full_pitch_deg = 60.0f; // pitch at which AimUp / AimDown have weight 1 (inferred, s3.md §5.5)
	bool run_noise = false; // add Rifle_Noise_RunFwd1 while running (off for the gates)
};

struct CrowdTeam {
	const SkinBinds *binds = nullptr; // the team's mesh binds (one list: body, head, helmet, weapon)
	const CrowdClipData *reload = nullptr; // load_chosen_clip for the team's weapon
	float reload_rate = 1.0f; // reload playback rate (1 = as authored; s3.md §5.4, §16 Q3)
	// Step 3 above: 1 for the 21 undriven Wep_* joints, and for each rig joint its weaponske01 LocalPose (identity
	// where the mask is 0). Both rig-sized; make() fails on a size mismatch. Null / empty: no fix.
	const BoneMask *weapon_joints = nullptr;
	std::span<const Xform> weapon_local;
};

// Per-frame counters of the last published job (reports and the G0 blend row, s3.md §1).
struct CrowdFrameStats {
	uint32_t evaluated = 0; // soldiers posed by the job
	uint32_t skipped_lod = 0; // due later (temporal LOD)
	uint32_t hidden = 0;
	// Run blend coverage over the evaluated soldiers.
	uint32_t loco_clips_sampled = 0; // locomotion clips (idle + cycles) actually sampled, summed over soldiers
	uint32_t two_cycle = 0; // soldiers with two direction cycles weighted
	uint32_t idle_mix = 0; // soldiers with idle and at least one cycle weighted
	uint32_t reloading = 0; // soldiers with reload weight > 0
	uint32_t aiming = 0; // soldiers with aim weight > 0
	// Evaluated soldiers per temporal LOD tier: index temporal_lod_tier() + 1 (0 = nearer than First, 1..6 =
	// First..Sixth, 7 = beyond far).
	std::array<uint32_t, TemporalLodTable::kTiers + 2> evaluated_by_tier{};
	double job_seconds = 0.0; // kick -> last item done (lane wall time)
	double blocked_seconds = 0.0; // wait_and_publish blocked on the job
};

class CrowdEvaluator {
public:
	// p_clips and the teams' binds and reload clips must outlive the evaluator. Fails when p_soldiers exceeds
	// kMaxCrowdSoldiers, a team has no binds or more than kMaxPaletteBinds, or a bind names a bone outside the rig.
	static core::Result<std::unique_ptr<CrowdEvaluator>> make(const CrowdClipSet &p_clips, std::span<const CrowdTeam> p_teams, uint32_t p_soldiers,
			const CrowdConfig &p_config);
	~CrowdEvaluator();
	CrowdEvaluator(const CrowdEvaluator &) = delete;
	CrowdEvaluator &operator=(const CrowdEvaluator &) = delete;

	uint32_t soldiers() const;
	// Owner thread, no job running. Inputs beyond soldiers() are ignored; missing ones keep their last value.
	// A non-finite position, velocity, angle or dt is replaced by its last finite value (or 0) and counted.
	void set_inputs(std::span<const CrowdSoldierInput> p_inputs, double p_dt);
	// Owner thread: starts the job on p_group (one item per soldier). False when a job is running.
	bool kick(core::WorkerGroup &p_group);
	// Owner thread: waits for the job, then flips front and back of every soldier the job evaluated (the others
	// keep their front). No-op without a job.
	void wait_and_publish(core::WorkerGroup &p_group);
	// Runs the whole job on the calling thread (tests, the single-thread bench row).
	void evaluate_all_here();

	// Front palette of soldier p_i: one Mat34 per bind of its team (model space; the root is not applied). Always the
	// palette on the GPU after the last upload (per-soldier double buffer, above).
	std::span<const Mat34> palette(uint32_t p_i) const;
	// True when the front palette of p_i changed in the last publish (temporal LOD skips and frozen frames leave
	// it false).
	bool changed(uint32_t p_i) const;
	// The rig model pose behind the front palette (pinch and hand-IK metrics, debug).
	std::span<const Xform> model(uint32_t p_i) const;
	const CrowdFrameStats &stats() const;
	uint64_t bad_inputs() const; // non-finite values replaced by set_inputs

private:
	CrowdEvaluator();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::anim
