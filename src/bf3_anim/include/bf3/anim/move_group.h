#pragma once

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

inline constexpr float kAntTicksPerSecond = 60.0f;
inline constexpr uint32_t kMaxAntRouteDepth = 8;
inline constexpr uint32_t kMaxMoveGroupDirections = 16;
inline constexpr uint32_t kMaxMoveGroupEntries = 8;
inline constexpr uint32_t kMaxMoveGroupClips = 32;
inline constexpr size_t kMaxMoveGroupTerms = 4;
inline constexpr float kMaxMoveGroupAngle = 25.2f;
inline constexpr bool kMoveGroupVelocityFromRoot = true;

enum class AntWeaponType : uint8_t { Rifle = 0, Hand = 1, Bags = 2, LMG = 3, AT = 4, Pistol = 5, SniperRifle = 6, Shotgun = 7, Unarmed = 8 };

struct ChooserIndices {
	uint32_t weapon_type = uint32_t(AntWeaponType::Rifle);
	int64_t specific_weapon = -1;
};

struct AntBranchTag {
	std::string type;
	float time = 0.0f, length = 0.0f;
	bool branch_in = false;
};

struct AntClipRoute {
	uint32_t clip_controller = 0;
	std::string controller;
	const core::ant::AntPackage *controller_package = nullptr;
	uint32_t anim_index = 0;
	uint32_t num_ticks = 0;
	float fps = 30.0f, fps_scale = 0.5f;

	float start_in_tick = 0.0f, end_in_tick = 0.0f;
	bool sequence = false;
	bool looping = false;
	std::vector<std::string> path;
	std::vector<AntBranchTag> branches;
	float loop_end_tick = 0.0f;
};

core::Result<AntClipRoute> route_ant_controller(const core::ant::AntLibrary &p_library, const core::ant::AntPackage &p_from, const core::ant::GdGuid &p_controller,
		const ChooserIndices &p_indices);

core::Result<AntClipRoute> route_ant_controller(const core::ant::AntLibrary &p_library, std::string_view p_type, std::string_view p_name,
		const ChooserIndices &p_indices);

core::Result<uint32_t> weapon_type_index(const core::ant::AntLibrary &p_library, uint32_t p_ant_ref_asset_id);

inline constexpr std::string_view kVmWeaponHandlingWrapper = "1P_WeaponHandlingWrapper";
inline constexpr std::string_view kHoldAndReleaseSignal = "Game.Weapon.FireHoldAndRelease";

struct AntHoldReleaseFlow {
	std::string name;
	AntClipRoute idle, start, hold, release, deploy;
	float release_from_tick = 0.0f, release_until_tick = 0.0f, release_in_tick = 0.0f;
	float hold_from_tick = 0.0f;
	float release_end_tick = 0.0f;
	float deploy_end_tick = 0.0f;
};

core::Result<AntHoldReleaseFlow> read_hold_release_flow(
		const core::ant::AntLibrary &p_library, std::string_view p_wrapper, const ChooserIndices &p_indices);

struct MoveGroupEntry {
	float velocity = 0.0f;
	float item_speed = 1.0f;
	float start_phase = 0.0f;
	float achieved_angle = 0.0f;
	uint32_t clip = 0;
};
struct MoveGroupDirection {
	float x = 0.0f;
	std::vector<MoveGroupEntry> entries;
};
struct MoveGroupDef {
	std::string name;
	std::vector<MoveGroupDirection> directions;
	float x_min = 0.0f, x_max = 0.0f;
	std::vector<AntClipRoute> clips;
	float one_to_one_speed = 0.0f, min_speed = 0.0f, master_length = 100.0f, direction_tolerance = 0.0f;
};

core::Result<MoveGroupDef> read_move_group(const core::ant::AntLibrary &p_library, std::string_view p_name, const ChooserIndices &p_indices);

struct MoveGroupClipTiming {
	float seconds = 0.0f;
	float root_velocity = 0.0f;
};
struct MoveGroupOptions {
	bool velocity_from_root = kMoveGroupVelocityFromRoot;
};
struct MoveGroupTerm {
	uint32_t clip = 0;
	float weight = 0.0f;
	float rate = 1.0f;
	float start_phase = 0.0f;
};
struct MoveGroupWeights {
	std::array<MoveGroupTerm, kMaxMoveGroupTerms> terms{};
	uint32_t count = 0;
	uint32_t moving = 0;
	float cycles_per_second = 0.0f;
	float x = 0.0f;
	float velocity = 0.0f;
	bool below_min_speed = false;
};

MoveGroupWeights move_group_weights(const MoveGroupDef &p_def, std::span<const MoveGroupClipTiming> p_timing, float p_x, float p_speed,
		const MoveGroupOptions &p_options = {});

float move_group_x(const Vec3 &p_local, float p_x_sign);

}
