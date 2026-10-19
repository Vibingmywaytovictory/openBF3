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
	// M2a w3 (W3.0 contract, docs/design/m2a.md §9; docs/design/m2a-w3.md): the server state's stance, weapon, fire,
	// ADS and death, which tp's R5 and Bf3GameCrowd drive. The defaults are S3's soldier (standing, the team's default
	// weapon, no fire), so S3's scenes and tests run unchanged. The wire's team 1 / 2 maps to team 0 / 1 here
	// (crowd_team_of_wire below).
	uint8_t pose = 0; // sim::Pose: 0 stand, 1 crouch, 2 prone
	uint8_t target_pose = 0; // the stance being entered
	float transition = 0.0f; // 0..1 from pose to target_pose (PresentedSoldier::transition)
	uint8_t weapon = 0; // index into the team's CrowdTeam::weapons (0 = the team's default weapon)
	uint8_t fire_seq = 0; // changes once per shot (or per burst seq change): the upper-body fire kick
	bool ads = false;
	bool dead = false; // blend to prone over 0.5 s, hidden after 3 s (m2a.md §16 Q13)
	// M2b W2.0 (docs/design/m2b.md §2.6 #17, §4; cam): this life began as a revive (PresentKind::Revive, Spawned::revived).
	// When dead falls to false with revived set, the soldier stands up from its corpse pose at the same place over
	// kCrowdReviveBlendSeconds instead of restarting alive at once. Default false: M2a and S3 unchanged.
	bool revived = false; // M2b W2.0 (cam)
	bool sprinting = false; // PT1 run [field]: the sim's kSprinting (stand draws MP_SprintMoveGroup with CrowdStanceSet::move_groups)
};

struct CrowdConfig {
	bool temporal_lod = true; // false: every soldier every frame (the G1 gate case, s3.md §1)
	TemporalLodTable lod; // read_game_animation_settings, or the defaults
	LocoParams loco;
	AdditiveOrder aim_order = AdditiveOrder::LocalPost; // s3.md §16 Q2
	// Pitch at which AimUp / AimDown have weight 1 (s3.md §5.5): the barrel elevation change the additive gives at
	// weight 1 (measure_aim; the defaults are the values measured on the retail clips with LocalPost, so weight =
	// |pitch| / full makes the barrel follow the pitch). Retail maps pitch through the Ant graph [inferred, §16 Q7].
	float aim_up_full_pitch_deg = 78.1f;
	float aim_down_full_pitch_deg = 60.7f;
	bool run_noise = false; // add Rifle_Noise_RunFwd1 while running (off for the gates)
};

// M2a w3 (W3.0 contract, docs/design/m2a.md §9; docs/design/m2a-w3.md): one 3p item a team can hold (tp fills and
// evaluates them, R5). The skinning step draws the body from CrowdTeam::binds and the weapon from the soldier's
// CrowdWeapon; the reload clip, the weapon joints' LocalPose and the 3p muzzle come per weapon.
inline constexpr uint32_t kMaxCrowdWeapons = 8; // 4 primaries, sidearm, knife, M67 per team, one spare
struct CrowdWeapon {
	const SkinBinds *binds = nullptr; // the 3p weapon mesh only (body, head, helmet stay in CrowdTeam::binds)
	const CrowdClipData *reload = nullptr; // S3's per-team reload, now per weapon (null: no reload clip)
	float reload_rate = 1.0f;
	const BoneMask *weapon_joints = nullptr; // step 3 above, per weapon (null / empty: no fix)
	std::span<const Xform> weapon_local; // weaponske01 LocalPose of this item, rig-sized
	int32_t muzzle_joint = -1; // rig joint the 3p muzzle hangs on (-1: none; the FX triggers then use the eye fallback)
	Xform muzzle_offset{}; // the muzzle in that joint's space (the effect frame: +Y along the barrel, m2a-fx2.md)
	uint8_t palette = 0; // material palette index of the 3p mesh
};

struct CrowdTeam {
	const SkinBinds *binds = nullptr; // the team's mesh binds (one list: body, head, helmet, weapon)
	const CrowdClipData *reload = nullptr; // load_chosen_clip for the team's weapon
	float reload_rate = 1.0f; // reload playback rate (1 = as authored; s3.md §5.4, §16 Q3)
	// Step 3 above: 1 for the 21 undriven Wep_* joints, and for each rig joint its weaponske01 LocalPose (identity
	// where the mask is 0). Both rig-sized; make() fails on a size mismatch. Null / empty: no fix.
	const BoneMask *weapon_joints = nullptr;
	std::span<const Xform> weapon_local;
	// M2a w3 (W3.0 contract, docs/design/m2a.md §9, R5): the items of this team (<= kMaxCrowdWeapons). Empty: the
	// fields above are the one weapon (S3). With items, CrowdSoldierInput::weapon picks one; an index past the end draws
	// item 0 and is counted.
	std::span<const CrowdWeapon> weapons;
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

// M2a tp [class] (named insertion, docs/design/m2a-tp.md): the stance set and the M2a counters of the members below
// (defined in the M2a tp block at the end of this file).
struct CrowdStanceSet;
struct CrowdM2aStats;

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
	// M2a w3 (W3.0 contract, docs/design/m2a.md §9; tp's body): the 3p muzzle of soldier p_i behind the front palette,
	// in model space (the root is not applied, as palette()): the weapon's muzzle joint times its muzzle offset. False
	// when the soldier's weapon has no muzzle joint, is hidden or was never evaluated. Published to the FX triggers
	// after wait_and_publish.
	bool muzzle_model(uint32_t p_i, Xform &r_model) const;
	// M2a w3 (W3.0 contract, docs/design/m2a.md §9; docs/design/m2a-w3.md §3 "tp's 3p weapon route"; tp's body, R5): the
	// soldier's weapon drawn as its own mesh. weapon_of(p_i) is the CrowdTeam::weapons index behind the front palette
	// (kNoCrowdWeapon when the team has no weapons, or the soldier is hidden or was never evaluated); weapon_palette(p_i)
	// is one Mat34 per bind of that CrowdWeapon::binds, model space (the root is not applied, as palette()), in the same
	// per-soldier double buffer and under the same changed(p_i) flag as palette(). Empty with kNoCrowdWeapon. palette(p_i)
	// keeps its layout: one Mat34 per CrowdTeam::binds entry (still body, head, helmet and the team's default weapon
	// binds; with weapons the default weapon's SoldierPart is stripped before upload, so no drawn vertex reads them).
	uint8_t weapon_of(uint32_t p_i) const;
	std::span<const Mat34> weapon_palette(uint32_t p_i) const;
	// M2a tp [class] (named insertion, docs/design/m2a-tp.md): make() with the stance set (lc's stance idles, aims and
	// transitions and tp's moves, fire and death clips): the soldiers then follow CrowdSoldierInput's pose, target_pose,
	// transition, fire_seq and dead (the M2a tp block at the end of this file). The make() above is this one with an empty
	// set. Fails as that one does, and when the stance clips' rig is not the crowd rig.
	static core::Result<std::unique_ptr<CrowdEvaluator>> make(const CrowdClipSet &p_clips, const CrowdStanceSet &p_stance, std::span<const CrowdTeam> p_teams,
			uint32_t p_soldiers, const CrowdConfig &p_config);
	// False when the soldier draws nothing: hidden (crowd_flags::kHidden), dead longer than kCrowdDeathHideSeconds, or never
	// evaluated. Owner thread, after wait_and_publish.
	bool drawn(uint32_t p_i) const;
	// What the last published job did with the M2a inputs, and the counted clamps since make().
	CrowdM2aStats m2a_stats() const;

private:
	CrowdEvaluator();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// ---- M2a w3 (W3.0 contract, docs/design/m2a.md §3, §16 Q11; docs/design/m2a-w3.md) --------------------------------

// The crowd's pose lanes: S3's clamp(hardware_concurrency - 4, 2, 6), and with a listen server in the process M2a's
// clamp(hardware_concurrency - 8, 2, kCrowdLanesListenMax) so the server thread keeps a core (a hardware_concurrency
// of 0 counts as 2 lanes). kCrowdLanesListenMax is 4 until the integrator's quiet-machine rerun of godot.s3_crowd
// with 4 lanes and a listen server; it becomes 5 if G1a's p99 there exceeds 2.0 ms (§3).
inline constexpr uint32_t kCrowdLanesListenMax = 4;
constexpr uint32_t crowd_lanes(uint32_t p_hardware_concurrency, bool p_listen_server) {
	if (p_hardware_concurrency == 0) {
		return 2;
	}
	const int64_t hc = int64_t(p_hardware_concurrency);
	const int64_t want = p_listen_server ? hc - 8 : hc - 4;
	const int64_t hi = p_listen_server ? int64_t(kCrowdLanesListenMax) : 6;
	return uint32_t(want < 2 ? 2 : (want > hi ? hi : want));
}
// The wire's team (net::OtherSoldier::team, Spawned::team: 1 US, 2 RU, 0 unknown) to CrowdSoldierInput::team (0 US,
// 1 RU); unknown draws as US.
constexpr uint8_t crowd_team_of_wire(uint8_t p_wire_team) {
	return p_wire_team == 2 ? uint8_t(1) : uint8_t(0);
}
// CrowdEvaluator::weapon_of when the soldier draws no separate weapon mesh (S3's teams without CrowdTeam::weapons,
// hidden or never evaluated).
inline constexpr uint8_t kNoCrowdWeapon = 0xFF;
static_assert(kMaxCrowdWeapons < kNoCrowdWeapon);

// ---- M2a tp (docs/design/m2a.md §9, R5, §16 Q13; docs/design/m2a-tp.md) ---------------------------------------------
//
// R5 (crowd_eval.cpp, tp): with CrowdTeam::weapons, a soldier's weapon layout (step 3), reload clip (step 4) and weapon
// binds (step 6) come from weapons[CrowdSoldierInput::weapon] (an index past the end draws item 0, counted); the team's
// own fields are the body (binds) and the weapon of a team without items. Per-soldier buffers are sized from the
// largest team's binds and the largest CrowdWeapon::binds, and a never-evaluated soldier's palette is empty (S3 sized
// it from team 0's binds).
//
// With a CrowdStanceSet (make() overload) the per-soldier steps become, in order:
//   1. stance and locomotion: dead -> the death pose below; pose != target_pose -> the transition clip (lc's Rifle_T_*)
//      at transition x its end frame, no locomotion (the capsule table's rule, m2a.md §6.5: from != to is a transition
//      at the fraction, even 0 and 1); otherwise the steady stance target_pose: stand = S3's run blend (bit for bit),
//      crouch / prone = their idle (lc's Rifle_CrouchIdle_Pose / Rifle_ProneIdle_Pose) and moves (tp's Sneak / Crawl
//      cycles, CrowdMoveClips::loco). A stance whose idle is missing is drawn from the stand idle, one whose moves are
//      incomplete moves with the stand cycles (both counted in CrowdM2aStats::stance_fallbacks).
//   2.-4. as S3 (root, the weapon's layout, the weapon's reload); a weapon change cancels a playing reload.
//   4b. fire: fire_seq changed -> crouch plays Rifle_CrouchFire from its start (additive when it decodes additive, else
//      a masked override ramped like the reload); stand and prone (no unique rifle fire controller, m2a.md §9) add
//      kCrowdFireKickDeg of aim pitch, a triangle over kCrowdFireKickSeconds [inferred].
//   5. aim: the stance's AimUp / AimDown additive at weight min(1, |pitch| / full), full = S3's config values for stand
//      and lc's server_pose_stance_aim for crouch and prone (the capsule table's weights); a transition aims with its
//      source stance (as the table); a dead soldier does not aim or fire.
//   6. body palette (team binds), weapon palette (CrowdWeapon::binds), muzzle (CrowdWeapon::muzzle_joint x
//      muzzle_offset) into the back buffers.
// Death (m2a.md §16 Q13, default BlendToProne): the pose blends from the alive pose to the prone idle over
// kCrowdDeathBlendSeconds; CrowdDeathStyle::Clip plays the stance's death clip (Death_StandChest, Death_Crouch1,
// Death_Prone1) and holds its last frame instead (BlendToProne when the clip is missing). Either way the soldier is hidden
// kCrowdDeathHideSeconds after `dead` rose (drawn() false, weapon_of kNoCrowdWeapon); dead falling back to false (a
// respawn) restarts it alive. Times run on the evaluator's input clock (set_inputs' dt), so temporal LOD skips do not
// stretch them.

struct ServerPoseClips; // bf3/anim/server_pose.h (lc): load_server_pose_clips with ServerPoseClipNames

enum class CrowdDeathStyle : uint8_t {
	BlendToProne = 0, // m2a.md §16 Q13's recommended default
	Clip = 1, // the stance's retail death clip, last frame held [verified present: CrowdMoveClips]
};
inline constexpr CrowdDeathStyle kCrowdDeathDefault = CrowdDeathStyle::BlendToProne; // §16 Q13
inline constexpr float kCrowdDeathBlendSeconds = 0.5f; // §16 Q13
inline constexpr float kCrowdDeathHideSeconds = 3.0f; // §16 Q13
inline constexpr float kCrowdFireKickSeconds = 0.05f; // m2a.md §9 [inferred]
inline constexpr float kCrowdFireKickDeg = 3.0f; // [inferred]

struct CrowdStanceSet {
	const ServerPoseClips *pose = nullptr; // lc's stance idles, aims and transitions (null: every stance drawn standing, counted)
	const CrowdMoveClips *moves = nullptr; // tp's moves, crouch fire and deaths (null: stand cycles, kicks, BlendToProne)
	CrowdDeathStyle death = kCrowdDeathDefault;
	// M2b W2.0 (docs/design/m2b.md §2.6 #17, §4; cam): seconds a corpse is drawn after `dead` rose (the hide above).
	// The default keeps M2a's kCrowdDeathHideSeconds; the M2b scene sets kCrowdCorpseSeconds (or SoldierDef::time_for_corpse).
	float corpse_seconds = kCrowdDeathHideSeconds; // M2b W2.0 (cam)
	const CrowdMoveGroupSet *move_groups = nullptr; // PT1 run [field]: BF3's move groups for stand and crouch (null: S3's / tp's cycles)
};

struct CrowdM2aStats {
	// The last published job, over the soldiers it evaluated.
	uint32_t stand = 0, crouch = 0, prone = 0; // steady stance drawn
	uint32_t transitions = 0; // posed from a transition clip (or its stand-in lerp)
	uint32_t moving_crouch = 0, moving_prone = 0; // a crouch / prone move cycle weighted
	uint32_t firing = 0; // a fire clip or kick weighted
	uint32_t dying = 0; // dead and still drawn
	uint32_t weapon_palettes = 0; // a weapon palette written (teams with CrowdTeam::weapons)
	uint32_t muzzles = 0; // a muzzle written
	// Since make(), over every evaluation.
	uint64_t stance_fallbacks = 0; // a crouch / prone / transition drawn from stand clips (missing stance clip)
	uint64_t bad_weapon = 0; // CrowdSoldierInput::weapon past the team's items (item 0 drawn)
	uint64_t bad_stance = 0; // pose / target_pose > 2 (clamped) or a non-finite transition (0)
	uint64_t reloads_cancelled = 0; // a weapon change during a reload
	uint64_t deaths = 0; // dead rose
	uint64_t death_hidden = 0; // soldiers hidden after kCrowdDeathHideSeconds (counted once per death)
	uint64_t revives = 0; // M2b cam [revive]: dead fell with CrowdSoldierInput::revived set (a stand-up started)
};

} // namespace bf3::anim

// ---- M2b W2.0 (docs/design/m2b.md §2.6 #17, §4; owner cam; contract: docs/design/m2b-w2.md) -----------------------
// The 3p corpse and the revive stand-up. kCrowdDeathHideSeconds (3 s) stays M2a's; the M2b scene keeps a corpse for
// kCrowdCorpseSeconds through CrowdStanceSet::corpse_seconds (cam's named replacement [corpse] at crowd_eval.cpp's
// hide test reads it). A revive (CrowdSoldierInput::revived) blends the corpse pose back to the alive stand over
// kCrowdReviveBlendSeconds (cam's [revive] insertion), at the same place. Ragdolls are out of M2b (§12 J3).

namespace bf3::anim {

inline constexpr float kCrowdCorpseSeconds = 15.0f; // SoldierDef::time_for_corpse on Metro (m2b.md §4) [named default]
inline constexpr float kCrowdReviveBlendSeconds = 0.5f; // prone -> stand (m2b.md §4) [inferred]

} // namespace bf3::anim

// M2b cam (docs/design/m2b-cam.md): CrowdEvaluator::make refuses a CrowdStanceSet::corpse_seconds that is not finite
// or outside [0, kCrowdMaxCorpseSeconds] (an error, never a clamp: the caller set it).
namespace bf3::anim {

inline constexpr float kCrowdMaxCorpseSeconds = 3600.0f;

} // namespace bf3::anim

// ---- PT1 run (docs/design/pt1-run.md) --------------------------------------------------------------------------------
// With CrowdStanceSet::move_groups, step 1's steady stand and crouch locomotion is BF3's move group (move_group.h)
// instead of S3's run blend / tp's cycles (the [movegroup] replacement in crowd_eval.cpp's loco_blend):
//   * stand: MP_RunMoveGroup_PC, and MP_SprintMoveGroup while CrowdSoldierInput::sprinting; crouch:
//     MP_SneakMoveGroup_PC. Prone keeps tp's Crawl cycles (BF3 has no prone move group). A group that did not load
//     (CrowdMoveGroup::ok false) keeps the stance's cycles.
//   * the terms of move_group_weights at the soldier's local velocity, sampled over the stance idle (the joints a clip
//     leaves undriven keep the idle, e.g. the fingers of the FullBodyWeaponRootNoFingers sprint), at one shared phase;
//   * the sprint weight ramps 0 <-> 1 over kCrowdSprintBlendSeconds (run and sprint blended, the phase rate blended
//     with them), and the moving weight (the group over the stance idle) over kCrowdMoveBlendSeconds: 0 below the
//     group's MinSpeed (MP_Run 0.01 m/tick = 0.6 m/s) unless sprinting, else 1.
// Without move_groups (S3's scenes, the M2a / M2b rows' synthetic stance sets) nothing changes.

namespace bf3::anim {

inline constexpr float kCrowdSprintBlendSeconds = 0.2f; // [named default, measured = false]
inline constexpr float kCrowdMoveBlendSeconds = 0.2f; // [named default, measured = false]

} // namespace bf3::anim
