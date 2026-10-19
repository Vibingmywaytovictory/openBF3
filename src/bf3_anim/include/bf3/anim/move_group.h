#pragma once
// PT1 run (docs/design/pt1-run.md): BF3's locomotion as its Ant data defines it. Godot-free.
//
// The 3p soldier's moves are StreetMoveGroupControllerAssets of s_basicassets [verified 2026-10-19, antdump objects
// --install s_basicassets StreetMoveGroupControllerAsset]: MP_RunMoveGroup_PC (stand), MP_SprintMoveGroup (sprint) and
// MP_SneakMoveGroup_PC (crouch); prone has none (it keeps the Crawl cycles of crowd_clips.h). A move group is
//   BlendArrayControllerAsset  an outer BlendArrayAsset over the movement angle: Items[] {Clip, X} with X in radians
//                              (MP_Run: -pi bwd, -pi/2 left, 0 fwd, pi/2 right, pi bwd, 3pi/2 left; Min/Max the range)
//     Items[].Clip             an inner BlendArrayAsset over the speed: Items[] {Clip, X} with X = the item's Velocity
//                              in metres per Ant tick (60 per second, kAntTicksPerSecond); X 0 is the zero-speed pose
//       Items[].Clip           a StreetMoveGroupItemControllerAsset: ControllerAsset (a clip controller or a chooser),
//                              ItemSpeed (playback rate), ItemStartPhase, ItemAchievedAngle, Velocity
//   OneToOneSpeed, MinSpeed, MasterLength, DirectionTolerance
// A chooser on the way is resolved with ChooserIndices: an IndexChooserControllerAsset reads its IndexAsset's
// GameStateEnumerationAsset (WeaponType_EnumGamestate -> weapon_type, SpecificWeapon_EnumGamestate -> specific_weapon,
// any other -> its Default); a ChooserControllerAsset with a SignalChooserPolicyAsset takes the first entry whose
// TrueSignals are all true and FalseSignals all false, every BoolAsset at its Default (SwitchLoco_SIG: false, so
// MP_RunLeftChooser plays Rifle_RunLeft1) [inferred: the signals' runtime values are not data].
//
// Evaluation (move_group_weights, data-free; the model is [inferred] from the data's shape, docs/design/pt1-run.md §3):
//   * the outer array: X = x_sign * heading of the local velocity (heading from +Z towards +X in rig model space),
//     wrapped into [Min, Min + 2 pi) and clamped to [Min, Max]; the two items whose X bracket it, linear weights;
//   * each inner array: v = speed / kAntTicksPerSecond; the two entries whose velocity bracket v, linear weights, each
//     played at its ItemSpeed; above the last entry that entry alone at ItemSpeed * (1 + OneToOneSpeed * (v / V - 1))
//     (one to one with the speed at OneToOneSpeed 1);
//   * one normalised phase for every clip of the group (ItemStartPhase / MasterLength added; BF3 authors the cycles
//     phase-aligned: every ItemStartPhase of the three groups is 0), advancing at the weighted mean of rate / seconds
//     of the moving terms.
// With MoveGroupOptions::velocity_from_root an entry's velocity is its clip's measured root speed times ItemSpeed (the
// stride on the ground) instead of the data's Velocity, so the feet do not slide where the two differ (the data's
// Velocity is root * ItemSpeed for Rifle_Sprint, Rifle_RunBwd and Rifle_RunLeft1 within 1 %, but 9 % below the root for
// Rifle_RunFwd; pt1-run.md §2) [named default kMoveGroupVelocityFromRoot, measured = false].

#include "bf3/anim/pose_math.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/ant/ant_package.h"
#include "bf3/core/ant/gd_archive.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::anim {

// Ant ticks per second: ClipControllerAsset FPS 30 / FPSScale 0.5 on every locomotion clip, NumTicks = 2 * EndFrame
// [verified 2026-10-19: Rifle_RunFwd NumTicks 36, EndFrame 18; ActorController Rifle_RunFwdSeq Length 36].
inline constexpr float kAntTicksPerSecond = 60.0f;
inline constexpr uint32_t kMaxAntRouteDepth = 8; // chooser / loop / layer / sequence hops to a clip controller
inline constexpr uint32_t kMaxMoveGroupDirections = 16;
inline constexpr uint32_t kMaxMoveGroupEntries = 8; // per direction
inline constexpr uint32_t kMaxMoveGroupClips = 32;
inline constexpr size_t kMaxMoveGroupTerms = 4; // two directions x two entries
inline constexpr float kMaxMoveGroupAngle = 25.2f; // |Min|, |Max| and every direction X, radians (four turns; the data uses -pi..1.5 pi)
inline constexpr bool kMoveGroupVelocityFromRoot = true; // [named default, measured = false] (above)

// WeaponTypeEnum of s_basicassets (the WeaponType_EnumGamestate enumeration) [verified 2026-10-19].
enum class AntWeaponType : uint8_t { Rifle = 0, Hand = 1, Bags = 2, LMG = 3, AT = 4, Pistol = 5, SniperRifle = 6, Shotgun = 7, Unarmed = 8 };

struct ChooserIndices {
	uint32_t weapon_type = uint32_t(AntWeaponType::Rifle); // WeaponType_EnumGamestate
	int64_t specific_weapon = -1; // SpecificWeapon_EnumGamestate (-1: the enumeration's Default)
};

// A controller followed through choosers, loops, layers and sequences to one ClipControllerAsset.
struct AntClipRoute {
	uint32_t clip_controller = 0; // guid index of the ClipControllerAsset
	std::string controller; // its __name
	const core::ant::AntPackage *controller_package = nullptr;
	uint32_t anim_index = 0; // its Anim reference's guid index (the clip in a bank package)
	uint32_t num_ticks = 0; // NumTicks
	float fps = 30.0f, fps_scale = 0.5f;
	// The clip ticks a sequence plays (ActorControllerAsset Tracks[0].Anims[0] StartInTime .. EndInTime), else
	// 0 .. num_ticks. Ticks are 1 / (fps / fps_scale) seconds.
	float start_in_tick = 0.0f, end_in_tick = 0.0f;
	bool sequence = false; // the window came from a sequence
	bool looping = false; // a LoopingControllerAsset was on the way
	std::vector<std::string> path; // "Type 'name'" per hop (reports)
};

// p_controller referenced from p_from (or the static package). Fails on an unknown controller type, an index outside
// a chooser, a signal chooser without a matching entry, more than kMaxAntRouteDepth hops or the work budget.
core::Result<AntClipRoute> route_ant_controller(const core::ant::AntLibrary &p_library, const core::ant::AntPackage &p_from, const core::ant::GdGuid &p_controller,
		const ChooserIndices &p_indices);
// The object of type p_type named p_name, unique in the static package, routed as above.
core::Result<AntClipRoute> route_ant_controller(const core::ant::AntLibrary &p_library, std::string_view p_type, std::string_view p_name,
		const ChooserIndices &p_indices);

// WeaponAsset (by its AntRef AssetId, crowd_clips.h's weapon_choice_index rule) -> WeaponClassEnum ->
// EnumerationValueAsset.Value: the WeaponTypeEnum index of a weapon (M16A2 -> Rifle 0).
core::Result<uint32_t> weapon_type_index(const core::ant::AntLibrary &p_library, uint32_t p_ant_ref_asset_id);

struct MoveGroupEntry {
	float velocity = 0.0f; // the inner X (= the item's Velocity), metres per Ant tick
	float item_speed = 1.0f;
	float start_phase = 0.0f; // ItemStartPhase
	float achieved_angle = 0.0f; // ItemAchievedAngle, radians
	uint32_t clip = 0; // index into MoveGroupDef::clips
};
struct MoveGroupDirection {
	float x = 0.0f; // the outer X, radians
	std::vector<MoveGroupEntry> entries; // ascending velocity
};
struct MoveGroupDef {
	std::string name;
	std::vector<MoveGroupDirection> directions; // ascending x
	float x_min = 0.0f, x_max = 0.0f; // the outer array's Min / Max
	std::vector<AntClipRoute> clips; // unique clip controllers, first use order
	float one_to_one_speed = 0.0f, min_speed = 0.0f, master_length = 100.0f, direction_tolerance = 0.0f;
};

// Reads the StreetMoveGroupControllerAsset p_name (unique in the static package). Fails on a missing or repeated name,
// a missing field, an empty or oversized array, non-finite numbers, an angle beyond kMaxMoveGroupAngle, or a route that
// fails (above).
core::Result<MoveGroupDef> read_move_group(const core::ant::AntLibrary &p_library, std::string_view p_name, const ChooserIndices &p_indices);

struct MoveGroupClipTiming {
	float seconds = 0.0f; // the clip's length at rate 1 (0: a pose, sampled at frame 0)
	float root_velocity = 0.0f; // its measured root speed, metres per Ant tick at rate 1 (0: unknown)
};
struct MoveGroupOptions {
	bool velocity_from_root = kMoveGroupVelocityFromRoot;
};
struct MoveGroupTerm {
	uint32_t clip = 0;
	float weight = 0.0f;
	float rate = 1.0f; // playback rate
	float start_phase = 0.0f; // added to the shared phase (0..1)
};
struct MoveGroupWeights {
	std::array<MoveGroupTerm, kMaxMoveGroupTerms> terms{}; // weights sum to 1; one term per clip
	uint32_t count = 0;
	uint32_t moving = 0; // terms with seconds > 0
	float cycles_per_second = 0.0f; // the shared phase's rate (0: no moving term, the phase holds)
	float x = 0.0f; // the outer X used
	float velocity = 0.0f; // metres per Ant tick used
	bool below_min_speed = false; // v < MinSpeed (the caller fades to its idle)
};
// The terms for the outer X p_x and the speed p_speed (m/s). p_timing has one entry per MoveGroupDef::clips (a shorter
// span counts the rest as poses). Non-finite inputs count as 0 (the zero-speed entry of the direction at X 0).
MoveGroupWeights move_group_weights(const MoveGroupDef &p_def, std::span<const MoveGroupClipTiming> p_timing, float p_x, float p_speed,
		const MoveGroupOptions &p_options = {});
// The outer X of a local horizontal velocity (rig model space): p_x_sign * atan2(x, z); 0 for a zero or non-finite one.
float move_group_x(const Vec3 &p_local, float p_x_sign);

} // namespace bf3::anim
