#pragma once

#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/layers.h"
#include "bf3/anim/move_group.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/core/ant/ant_library.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::anim {

inline constexpr size_t kVaultAnimKinds = 3;
inline constexpr std::string_view kVaultSignalOwner = "PhysicsMotion";
inline constexpr std::array<std::string_view, kVaultAnimKinds> kVaultTriggerFields = { "SIGVaultHigh", "SIGVaultUpHigh",
	"SIGVaultLow" };
inline constexpr std::array<std::string_view, kVaultAnimKinds> kVaultOutFields = { "SIGOutVaultHigh",
	"SIGOutVaultUpHigh", "SIGOutVaultLow" };

struct VaultController {
	core::ant::GdGuid guid;
	const core::ant::AntPackage *package = nullptr;
	std::string name;
	std::string node;
	float blend_in_ticks = 0.0f;
};

struct VaultControllers {
	std::array<core::ant::GdGuid, kVaultAnimKinds> triggers{};
	std::array<std::string, kVaultAnimKinds> trigger_names;
	std::array<VaultController, kVaultAnimKinds> first_person;
	std::array<VaultController, kVaultAnimKinds> third_person;
};

core::Result<VaultControllers> find_vault_controllers(const core::ant::AntLibrary &p_library);

struct VaultPlay {
	float start_tick = 0.0f;
	float end_tick = 0.0f;
	float clip_ticks_per_tick = 1.0f;
	float frames_per_tick = 0.5f;

	float seconds() const { return (end_tick - start_tick) / kAntTicksPerSecond; }
	float frame_at(float p_seconds) const {
		return (start_tick + p_seconds * kAntTicksPerSecond) * clip_ticks_per_tick * frames_per_tick;
	}
};

core::Result<VaultPlay> vault_play_of(const AntClipRoute &p_route);

core::Result<AntClipRoute> route_vault(
		const core::ant::AntLibrary &p_library, const VaultController &p_controller, const ChooserIndices &p_indices);

struct CrowdVaultClip {
	CrowdClipData clip;
	VaultPlay play;
	float blend_in_seconds = 0.0f;
	Vec3 root_start{};
	std::string controller;
};

core::Result<CrowdVaultClip> load_crowd_vault(const CrowdClipSet &p_set, const core::ant::AntLibrary &p_library,
		const VaultControllers &p_controllers, size_t p_kind, const ChooserIndices &p_indices);

}
