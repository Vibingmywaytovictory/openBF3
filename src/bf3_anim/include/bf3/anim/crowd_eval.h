#pragma once

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

constexpr uint32_t kMaxCrowdSoldiers = 256;
constexpr uint32_t kMaxCrowdTeams = 2;
constexpr uint32_t kMaxPaletteBinds = 256;

namespace crowd_flags {
constexpr uint8_t kReloadStart = 1 << 0;
constexpr uint8_t kHidden = 1 << 1;
}

struct CrowdSoldierInput {
	Vec3 position{};
	Vec3 velocity{};
	float yaw = 0.0f;
	float pitch = 0.0f;
	float camera_distance = 0.0f;
	uint8_t team = 0;
	uint8_t flags = 0;

	uint8_t pose = 0;
	uint8_t target_pose = 0;
	float transition = 0.0f;
	uint8_t weapon = 0;
	uint8_t fire_seq = 0;
	bool ads = false;
	bool dead = false;

	bool revived = false;
	bool sprinting = false;
	bool holding = false;
};

struct CrowdConfig {
	bool temporal_lod = true;
	TemporalLodTable lod;
	LocoParams loco;
	AdditiveOrder aim_order = AdditiveOrder::LocalPost;

	float aim_up_full_pitch_deg = 78.1f;
	float aim_down_full_pitch_deg = 60.7f;
	bool run_noise = false;
};

inline constexpr uint32_t kMaxCrowdWeapons = 254;
struct CrowdWeapon {
	const SkinBinds *binds = nullptr;
	const CrowdThrowClips *throw_clips = nullptr;
	const CrowdClipData *reload = nullptr;
	float reload_rate = 1.0f;
	const BoneMask *weapon_joints = nullptr;
	std::span<const Xform> weapon_local;
	int32_t muzzle_joint = -1;
	Xform muzzle_offset{};
	uint8_t palette = 0;
};

struct CrowdTeam {
	const SkinBinds *binds = nullptr;
	const CrowdClipData *reload = nullptr;
	float reload_rate = 1.0f;

	const BoneMask *weapon_joints = nullptr;
	std::span<const Xform> weapon_local;

	std::span<const CrowdWeapon> weapons;
};

struct CrowdFrameStats {
	uint32_t evaluated = 0;
	uint32_t skipped_lod = 0;
	uint32_t hidden = 0;

	uint32_t loco_clips_sampled = 0;
	uint32_t two_cycle = 0;
	uint32_t idle_mix = 0;
	uint32_t reloading = 0;
	uint32_t aiming = 0;

	std::array<uint32_t, TemporalLodTable::kTiers + 2> evaluated_by_tier{};
	double job_seconds = 0.0;
	double blocked_seconds = 0.0;
};

struct CrowdStanceSet;
struct CrowdCombatStats;

class CrowdEvaluator {
public:

	static core::Result<std::unique_ptr<CrowdEvaluator>> make(const CrowdClipSet &p_clips, std::span<const CrowdTeam> p_teams, uint32_t p_soldiers,
			const CrowdConfig &p_config);
	~CrowdEvaluator();
	CrowdEvaluator(const CrowdEvaluator &) = delete;
	CrowdEvaluator &operator=(const CrowdEvaluator &) = delete;

	uint32_t soldiers() const;

	void set_inputs(std::span<const CrowdSoldierInput> p_inputs, double p_dt);

	bool kick(core::WorkerGroup &p_group);

	void wait_and_publish(core::WorkerGroup &p_group);

	void evaluate_all_here();

	std::span<const Mat34> palette(uint32_t p_i) const;

	bool changed(uint32_t p_i) const;

	std::span<const Xform> model(uint32_t p_i) const;
	const CrowdFrameStats &stats() const;
	uint64_t bad_inputs() const;

	bool muzzle_model(uint32_t p_i, Xform &r_model) const;

	core::Result<uint8_t> add_weapon(uint32_t p_team, const CrowdWeapon &p_weapon);
	uint8_t weapon_of(uint32_t p_i) const;
	std::span<const Mat34> weapon_palette(uint32_t p_i) const;

	static core::Result<std::unique_ptr<CrowdEvaluator>> make(const CrowdClipSet &p_clips, const CrowdStanceSet &p_stance, std::span<const CrowdTeam> p_teams,
			uint32_t p_soldiers, const CrowdConfig &p_config);

	bool drawn(uint32_t p_i) const;

	CrowdCombatStats combat_stats() const;

private:
	CrowdEvaluator();
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

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

constexpr uint8_t crowd_team_of_wire(uint8_t p_wire_team) {
	return p_wire_team == 2 ? uint8_t(1) : uint8_t(0);
}

inline constexpr uint8_t kNoCrowdWeapon = 0xFF;
static_assert(kMaxCrowdWeapons < kNoCrowdWeapon);

struct ServerPoseClips;

enum class CrowdDeathStyle : uint8_t {
	BlendToProne = 0,
	Clip = 1,
};
inline constexpr CrowdDeathStyle kCrowdDeathDefault = CrowdDeathStyle::BlendToProne;
inline constexpr float kCrowdDeathBlendSeconds = 0.5f;
inline constexpr float kCrowdDeathHideSeconds = 3.0f;
inline constexpr float kCrowdFireKickSeconds = 0.05f;
inline constexpr float kCrowdFireKickDeg = 3.0f;
inline constexpr float kCrowdLayerRampSeconds = 0.15f;

struct CrowdStanceSet {
	const ServerPoseClips *pose = nullptr;
	const CrowdMoveClips *moves = nullptr;
	CrowdDeathStyle death = kCrowdDeathDefault;

	float corpse_seconds = kCrowdDeathHideSeconds;
	const CrowdMoveGroupSet *move_groups = nullptr;
};

struct CrowdCombatStats {

	uint32_t stand = 0, crouch = 0, prone = 0;
	uint32_t transitions = 0;
	uint32_t moving_crouch = 0, moving_prone = 0;
	uint32_t firing = 0;
	uint32_t throwing = 0;
	uint32_t dying = 0;
	uint32_t weapon_palettes = 0;
	uint32_t muzzles = 0;

	uint64_t stance_fallbacks = 0;
	uint64_t bad_weapon = 0;
	uint64_t bad_stance = 0;
	uint64_t reloads_cancelled = 0;
	uint64_t deaths = 0;
	uint64_t death_hidden = 0;
	uint64_t revives = 0;
};

}

namespace bf3::anim {

inline constexpr float kCrowdCorpseSeconds = 15.0f;
inline constexpr float kCrowdReviveBlendSeconds = 0.5f;

}

namespace bf3::anim {

inline constexpr float kCrowdMaxCorpseSeconds = 3600.0f;

}

namespace bf3::anim {

inline constexpr float kCrowdSprintBlendSeconds = 0.2f;
inline constexpr float kCrowdMoveBlendSeconds = 0.2f;

}
