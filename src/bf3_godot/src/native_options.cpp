#include "native_options.h"

#include "native_screens.h"

#include "bf3/core/ui/options.h"
#include "bf3/core/ui/settings_effects.h"
#include "bf3/core/util/key_values.h"
#include "bf3/host/ui_flow_session.h"

#include <cmath>
#include <optional>
#include <string>

namespace bf3::godot_side {

namespace {

namespace ui = bf3::core::ui;

const char *flag(bool p_value) {
	return p_value ? "True" : "False";
}

std::optional<double> number_of(std::string_view p_text) {
	if (p_text.starts_with("ID")) {
		p_text.remove_prefix(2);
	}
	double v = 0.0;
	if (!bf3::core::parse_finite(p_text, v)) {
		return std::nullopt;
	}
	return v;
}

std::optional<uint8_t> index_of(std::string_view p_text, double p_limit) {
	const std::optional<double> v = number_of(p_text);
	if (!v || *v < 0.0 || *v >= p_limit || std::floor(*v) != *v) {
		return std::nullopt;
	}
	return uint8_t(*v);
}

}

void NativeOptions::build(Bf3UiScreens &r_screens) {
	for (const std::string_view p : ui::kOptionsScreens) {
		r_screens.set_drawn(p, true);
	}
	r_screens.set_drawn(kPopupBindKeyPartition, true);
	r_screens.set_options(&model_);
}

void NativeOptions::feed(bf3::host::UiFlowSession &r_session, const ui::OptionsSession &p_options) {
	const ui::SettingsStore &s = p_options.store();
	r_session.set_data(kOptionsSettingsCategory, ui::kOptionsSettingsChangedKey, flag(s.settings_changed()));
	r_session.set_data(kOptionsSettingsCategory, ui::kOptionsVideoChangedKey, flag(s.video_settings_changed()));
	r_session.set_data(
			kOptionsSettingsCategory, ui::kOptionsFullscreenKey, flag(s.applied_value(ui::kFullscreenOption) != 0.0));
	r_session.set_data(kOptionsMemCardCategory, ui::kOptionsCanSaveKey, flag(true));
}

void NativeOptions::apply_writes(bf3::host::UiFlowSession &r_session, ui::OptionsSession &r_options) {
	for (const bf3::host::UiFlowDataWrite &w : r_session.take_data_writes()) {
		if (w.category != kOptionsSettingsLeaf) {
			continue;
		}
		switch (w.key) {
			case ui::kOptionsTabKey:
				if (const std::optional<uint8_t> t = index_of(w.value, double(ui::kOptionsTabCount))) {
					r_options.select_tab(*t);
					++stats_.tabs;
				}
				break;
			case ui::kOptionsKeyTabKey:
				if (const std::optional<uint8_t> t =
								index_of(w.value, double(r_options.store().key_bindings().tabs().size()))) {
					r_options.select_key_tab(*t);
					++stats_.key_tabs;
				}
				break;
			case ui::kOptionsKeyColumnKey:
				if (const std::optional<uint8_t> c = index_of(w.value, double(ui::kKeyColumnCount))) {
					r_options.select_key_column(*c);
				}
				break;
			case ui::kOptionsKeyRowKey:
				if (const std::optional<double> r = number_of(w.value); r && *r >= 0.0 && *r < 65536.0) {
					row_ = uint16_t(*r);
				}
				break;
			case ui::kOptionsValueKey:
				pending_value_ = number_of(w.value).value_or(std::numeric_limits<double>::quiet_NaN());
				break;
			case ui::kOptionsDataKeyKey:
				if (const std::optional<double> k = number_of(w.value); k && std::isfinite(pending_value_)) {
					++(r_options.set_option_value(int32_t(*k), pending_value_) ? stats_.values : stats_.refused_values);
				}
				break;
			case ui::kOptionsExitingKey:
				if (ui::ui_flow_truthy(w.value)) {
					r_options.exit();
					inside_ = false;
					capturing_ = false;
					++stats_.exited;
				} else {
					r_options.enter();
					inside_ = true;
					++stats_.entered;
				}
				break;
			default:
				break;
		}
	}
}

bool NativeOptions::take(
		const ui::UiFlowAction &p_action, bf3::host::UiFlowSession &r_session, ui::OptionsSession &r_options) {
	const ui::OptionsAction a = ui::options_action_of(p_action.key, p_action.params);
	switch (a) {
		case ui::OptionsAction::None:
			return false;
		case ui::OptionsAction::BindKey:
			r_session.erase_data(kOptionsSettingsCategory, ui::kOptionsKeyBoundKey);
			capturing_ = r_options.bind_key(row_, ui::KeyColumn(r_options.key_column()));
			stats_.captures += capturing_ ? 1 : 0;
			return true;
		case ui::OptionsAction::ConfirmBind:
		case ui::OptionsAction::RevertBind:
			capturing_ = false;
			stats_.confirms += r_options.run(a) ? 1 : 0;
			return true;
		case ui::OptionsAction::Save:
			++stats_.saves;
			break;
		case ui::OptionsAction::Revert:
			++stats_.reverts;
			break;
		case ui::OptionsAction::Reset:
			++stats_.resets;
			break;
		case ui::OptionsAction::ApplyVideo:
			++stats_.video_applies;
			break;
		case ui::OptionsAction::RevertVideo:
			++stats_.video_reverts;
			break;
	}
	r_options.run(a);
	return true;
}

bool NativeOptions::key(std::string_view p_input, bf3::host::UiFlowSession &r_session, ui::OptionsSession &r_options) {
	if (!capturing_) {
		return false;
	}
	switch (r_options.key_pressed(p_input)) {
		case ui::KeyBindResult::Bound:
			++stats_.keys_bound;
			r_session.set_data(kOptionsSettingsCategory, ui::kOptionsKeyBoundKey, flag(false));
			capturing_ = false;
			break;
		case ui::KeyBindResult::Conflict:
			++stats_.conflicts;
			r_session.set_data(kOptionsSettingsCategory, ui::kOptionsKeyBoundKey, flag(true));
			capturing_ = false;
			break;
		case ui::KeyBindResult::Cancelled:
		case ui::KeyBindResult::Cleared:
			++stats_.cancels;
			r_session.set_data(kOptionsSettingsCategory, ui::kOptionsKeyBoundKey, flag(false));
			capturing_ = false;
			break;
		case ui::KeyBindResult::Refused:
			++stats_.refused_keys;
			break;
	}
	return true;
}

void NativeOptions::update_model(const ui::OptionsSession &p_options) {
	model_ = p_options.model();
}

}
