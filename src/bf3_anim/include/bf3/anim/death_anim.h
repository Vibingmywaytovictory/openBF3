#pragma once

#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/move_group.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/vault_anim.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::anim {

inline constexpr std::string_view kRagdollBlendSignal = "Game.Character.RagdollBlendValue.Out";
inline constexpr std::string_view kDeathExplosionSignal = "Game.Character.Death.Explosion";
inline constexpr std::string_view kDeathProneSignal = "Game.Character.Prone";
inline constexpr std::string_view kDeathCrouchSignal = "Game.Character.Crouch";
inline constexpr std::string_view kDeathRandomValueSignal = "Game.Character.Death.RandomValue";
inline constexpr uint8_t kNoCrowdDeathClip = 0xFF;

struct CrowdDeathClip {
	CrowdClipData clip;
	VaultPlay play;
	std::vector<AntFloatStateTag> ragdoll;
	std::string controller;

	float ragdoll_target(float p_seconds) const;
};

enum class CrowdDeathNode : uint8_t { Explosion, Prone, Crouch, Stand };

struct CrowdDeathChoice {
	bool explosion = false;
	bool prone = false;
	bool crouch = false;
	float random_value = 1.0f;
};

struct CrowdDeathClips {
	std::vector<CrowdDeathClip> clips;
	std::vector<CrowdDeathNode> order;
	uint8_t prone = kNoCrowdDeathClip;
	std::vector<std::pair<float, uint8_t>> crouch;

	CrowdDeathNode node(const CrowdDeathChoice &p_choice) const;
	uint8_t choose(const CrowdDeathChoice &p_choice) const;
};

core::Result<CrowdDeathClips> load_crowd_death_clips(
		const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library, const ChooserIndices &p_indices);

struct RagdollRig {
	std::vector<int32_t> joints;
	std::vector<Xform> bone_to_body;

	static core::Result<RagdollRig> make(
			const Skeleton &p_rig, std::span<const std::string> p_bones, std::span<const Vec3> p_pivots);

	void bodies(std::span<const Xform> p_model, const Xform &p_root, std::span<Xform> r_bodies) const;
	void joints_model(std::span<const Xform> p_bodies, const Xform &p_root, std::span<Xform> r_model) const;
};

void ragdoll_local_pose(const Skeleton &p_rig, const LocalPose &p_pose, std::span<const int32_t> p_joints,
		std::span<const Xform> p_model, LocalPose &r_out);

}
