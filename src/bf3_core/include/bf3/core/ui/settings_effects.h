#pragma once

#include "bf3/core/audio/sound_program.h"
#include "bf3/core/ui/key_bindings.h"
#include "bf3/core/ui/settings_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

struct ActionConcept {
	std::string_view action;
	std::string_view map;
	std::string_view concept_id;
	bool negative = false;
};

inline constexpr ActionConcept kActionConcepts[] = {
	{ "bf3_forward", "infantry", "ConceptMoveFB", false },
	{ "bf3_back", "infantry", "ConceptMoveFB", true },
	{ "bf3_left", "infantry", "ConceptMoveLR", true },
	{ "bf3_right", "infantry", "ConceptMoveLR", false },
	{ "bf3_sprint", "infantry", "ConceptSprint", false },
	{ "bf3_jump", "infantry", "ConceptJump", false },
	{ "bf3_crouch", "infantry", "ConceptCrouch", false },
	{ "bf3_crouch_hold", "infantry", "ConceptCrouchOnHold", false },
	{ "bf3_prone", "infantry", "ConceptProne", false },
	{ "bf3_fire", "infantry", "ConceptFire", false },
	{ "bf3_ads", "infantry", "ConceptZoom", false },
	{ "bf3_reload", "infantry", "ConceptReload", false },
	{ "bf3_primary", "infantry", "ConceptSelectInventoryItem1", false },
	{ "bf3_sidearm", "infantry", "ConceptSelectInventoryItem2", false },
	{ "bf3_gadget", "infantry", "ConceptSwitchToStaticGadget", false },
	{ "bf3_grenade", "infantry", "ConceptThrowGrenade", false },
	{ "bf3_knife", "infantry", "ConceptMeleeAttack", false },
	{ "bf3_fire_mode", "infantry", "ConceptCycleFireMode", false },
	{ "bf3_scoreboard", "default", "ConceptScoreboard", false },
};

const ActionConcept *action_concept(std::string_view p_action);
std::vector<std::string> action_inputs(const KeyBindingTable &p_table, const ActionConcept &p_action);

inline constexpr std::string_view kVolumeOption = "Volume";
inline constexpr std::string_view kMusicVolumeOption = "MusicVolume";
inline constexpr std::string_view kDialogueVolumeOption = "DialogueVolume";
inline constexpr std::string_view kMouseSensitivityOption = "MouseSensitivity";
inline constexpr std::string_view kFlipLookOption = "Scheme0FlipY";
inline constexpr std::string_view kHoldToZoomOption = "HoldButtonToZoom";
inline constexpr std::string_view kFullscreenOption = "FullscreenEnabled";
inline constexpr std::string_view kMonitorOption = "FullscreenScreen";
inline constexpr std::string_view kVSyncOption = "VSync";
inline constexpr std::string_view kFieldOfViewOption = "FieldOfView";

inline constexpr size_t kMasterBusCount = size_t(audio::MasterBus::Unknown);

struct VideoEffects {
	bool fullscreen = false;
	uint32_t monitor = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool vsync = false;
	bool operator==(const VideoEffects &) const = default;
};

struct SettingsEffects {
	std::array<float, kMasterBusCount> bus_gains{};
	float look_scale = 1.0f;
	float hip_fov_deg = 70.0f;
	bool invert_look = false;
	bool hold_to_zoom = true;
	VideoEffects video;
};

SettingsEffects settings_effects(const SettingsStore &p_store);

}
