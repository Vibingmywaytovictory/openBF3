#pragma once
// Pose evaluation of the 3p crowd: per soldier, the run blend, a masked upper-body reload, the additive aim
// and the skin palette, written on worker lanes and published once per frame (S3; docs/design/s3.md §3, §5).
//
// Frame protocol (one owner thread, the Godot main thread in S3; docs/design/s3.md §3):
//   evaluator.wait_and_publish(group)       // job of the previous frame done: its palettes become front
//   ... upload the front palettes ...       // main thread, Godot
//   evaluator.set_inputs(inputs, dt)        // this frame's sim inputs
//   evaluator.kick(group)                   // evaluate every due soldier into the back palettes
// So the pose a frame shows was evaluated from the inputs of the frame before it (one frame of pose latency).
// The root transform is not part of the palette: the main thread sets it from the current input, so the
// soldier's position is never late.
//
// Per soldier, in order (CrowdClip and LocoClip names):
//   1. loco = blend of up to two direction cycles + idle at one phase (locomotion_weights)
//   2. the root joint's translation reset to rest (the sim owns the root)
//   3. reload: override_masked(loco, reload sample, reload mask, w), w ramps 0.15 s in and out
//   4. aim: apply_additive(loco, AimUp or AimDown at |pitch| / 60 deg, aim mask, order)
//   5. model_pose -> skin_matrices(binds of the soldier's team) -> back palette
// Temporal LOD (temporal_lod.h) skips steps 1-5 for a soldier whose interval has not elapsed. Its palette is
// then not marked changed and the uploader skips it. Updates are staggered by slot, so the due soldiers of a
// tier spread over frames.

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
};

// Per-frame counters of the last published job (reports).
struct CrowdFrameStats {
	uint32_t evaluated = 0; // soldiers posed by the job
	uint32_t skipped_lod = 0; // due later (temporal LOD)
	uint32_t hidden = 0;
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
	// Owner thread: waits for the job, then swaps back and front palettes. No-op without a job.
	void wait_and_publish(core::WorkerGroup &p_group);
	// Runs the whole job on the calling thread (tests, the single-thread bench row).
	void evaluate_all_here();

	// Front palette of soldier p_i: one Mat34 per bind of its team (model space; the root is not applied).
	std::span<const Mat34> palette(uint32_t p_i) const;
	// True when the front palette of p_i changed in the last publish (temporal LOD skips leave it false).
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
