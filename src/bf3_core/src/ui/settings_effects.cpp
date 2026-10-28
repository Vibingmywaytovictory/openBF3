#include "bf3/core/ui/settings_effects.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::ui {

namespace {

float gain_of(double p_value) {
	return std::isfinite(p_value) ? float(std::clamp(p_value, 0.0, 1.0)) : 1.0f;
}

}

const ActionConcept *action_concept(std::string_view p_action) {
	for (const ActionConcept &a : kActionConcepts) {
		if (a.action == p_action) {
			return &a;
		}
	}
	return nullptr;
}

std::vector<std::string> action_inputs(const KeyBindingTable &p_table, const ActionConcept &p_action) {
	std::vector<std::string> out;
	const KeyBindingRow *r = p_table.row(p_action.map, p_action.concept_id, p_action.negative);
	if (r == nullptr) {
		return out;
	}
	for (const KeyColumn c : { KeyColumn::Keyboard, KeyColumn::Mouse }) {
		const std::string &input = r->inputs[size_t(c)];
		if (input_is_bound(input)) {
			out.push_back(input);
		}
	}
	return out;
}

SettingsEffects settings_effects(const SettingsStore &p_store) {
	SettingsEffects e;
	const float master = gain_of(p_store.value(kVolumeOption));
	const float music = gain_of(p_store.value(kMusicVolumeOption));
	const float dialogue = gain_of(p_store.value(kDialogueVolumeOption));
	e.bus_gains.fill(master);
	for (const audio::MasterBus b : { audio::MasterBus::Music, audio::MasterBus::LoadingMusic }) {
		e.bus_gains[size_t(b)] = master * music;
	}
	for (const audio::MasterBus b : { audio::MasterBus::Dialog, audio::MasterBus::NarrVoice }) {
		e.bus_gains[size_t(b)] = master * dialogue;
	}
	const double sensitivity_default = p_store.default_value(kMouseSensitivityOption);
	e.look_scale = sensitivity_default > 0.0
			? float(std::max(0.0, p_store.value(kMouseSensitivityOption)) / sensitivity_default)
			: 1.0f;
	const double fov = p_store.value(kFieldOfViewOption);
	const double fov_default = p_store.default_value(kFieldOfViewOption);
	e.hip_fov_deg = float(std::isfinite(fov) && fov > 0.0 ? fov : fov_default > 0.0 ? fov_default : e.hip_fov_deg);
	e.safe_width = p_store.value(kSafeAreaWidthOption);
	e.safe_height = p_store.value(kSafeAreaHeightOption);
	e.invert_look = p_store.value(kFlipLookOption) != 0.0;
	e.hold_to_zoom = p_store.value(kHoldToZoomOption) != 0.0;
	e.video.fullscreen = p_store.applied_value(kFullscreenOption) != 0.0;
	e.video.monitor = uint32_t(std::max(0.0, p_store.applied_value(kMonitorOption)));
	e.video.width = uint32_t(std::max(0.0, p_store.applied_value(kResolutionWidth)));
	e.video.height = uint32_t(std::max(0.0, p_store.applied_value(kResolutionHeight)));
	e.video.vsync = p_store.value(kVSyncOption) != 0.0;
	return e;
}

}
