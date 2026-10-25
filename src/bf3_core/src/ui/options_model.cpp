#include "bf3/core/ui/options_model.h"

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/ui_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bf3::core::ui {

namespace {

constexpr int32_t kSaveOrRevertOptionsKey = -2113864226;
constexpr int32_t kBindKeyKey = -164353320;

std::string snake(std::string_view p_source) {
	char buf[128] = {};
	return std::string(buf, hud_snake_case(p_source, buf, sizeof(buf)));
}

std::string_view first_param(std::span<const std::string> p_params) {
	return p_params.empty() ? std::string_view() : std::string_view(p_params[0]);
}

}

std::string display_mode_label(const DisplayModeChoice &p_mode) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%ux%u %.2fHz", p_mode.width, p_mode.height, double(p_mode.hertz));
	return buf;
}

const OptionRowData *OptionsModel::row_of_key(int32_t p_data_key) const {
	const auto it =
			std::find_if(rows.begin(), rows.end(), [&](const OptionRowData &r) { return r.data_key == p_data_key; });
	return it == rows.end() ? nullptr : &*it;
}

const OptionRowData *OptionsModel::row_of_field(std::string_view p_field) const {
	const auto it = std::find_if(rows.begin(), rows.end(), [&](const OptionRowData &r) { return r.field == p_field; });
	return it == rows.end() ? nullptr : &*it;
}

bool OptionsModel::has_field(std::string_view p_field) const {
	return row_of_field(p_field) != nullptr ||
			std::find(std::begin(kOptionsStateFields), std::end(kOptionsStateFields), p_field) !=
			std::end(kOptionsStateFields);
}

OptionsAction options_action_of(int32_t p_action_key, std::span<const std::string> p_params) {
	const std::string_view p = first_param(p_params);
	if (p_action_key == kSaveOrRevertOptionsKey) {
		return p == "SAVE" ? OptionsAction::Save : p == "REVERT" ? OptionsAction::Revert : OptionsAction::None;
	}
	if (p_action_key == ui_action_key("ApplyVideoSettings")) {
		return p == "APPLY"		? OptionsAction::ApplyVideo
				: p == "REVERT" ? OptionsAction::RevertVideo
								: OptionsAction::None;
	}
	if (p_action_key == ui_action_key("ResetOptions")) {
		return OptionsAction::Reset;
	}
	if (p_action_key == kBindKeyKey) {
		return OptionsAction::BindKey;
	}
	if (p_action_key == ui_action_key("ConfirmKeyBind")) {
		return p == "1" ? OptionsAction::ConfirmBind : p == "0" ? OptionsAction::RevertBind : OptionsAction::None;
	}
	return OptionsAction::None;
}

OptionsSession::OptionsSession(const OptionsCatalog &p_catalog, KeyBindingTable p_bindings) :
		store_(p_catalog, std::move(p_bindings)) {}

void OptionsSession::enter() {
	exiting_ = false;
	capture_.cancel();
}

void OptionsSession::exit() {
	exiting_ = true;
	capture_.cancel();
}

void OptionsSession::select_tab(uint8_t p_tab) {
	selected_tab_ = p_tab;
}

void OptionsSession::select_key_tab(uint8_t p_tab) {
	if (p_tab < store_.key_bindings().tabs().size()) {
		key_tab_ = p_tab;
		capture_.cancel();
	}
}

bool OptionsSession::set_option_value(int32_t p_data_key, double p_value) {
	const OptionSource *src = option_source_of_key(p_data_key);
	const OptionSetting *s =
			src == nullptr || store_.catalog() == nullptr ? nullptr : store_.catalog()->setting(src->source);
	if (s == nullptr || !std::isfinite(p_value)) {
		return false;
	}
	if (s->kind == OptionKind::DisplayMode) {
		const size_t i = size_t(p_value);
		if (p_value < 0.0 || std::floor(p_value) != p_value || i >= runtime_.display_modes.size()) {
			return false;
		}
		const DisplayModeChoice &m = runtime_.display_modes[i];
		return store_.set_display_mode(m.width, m.height, m.hertz);
	}
	if (s->kind == OptionKind::Monitor) {
		return p_value < double(runtime_.monitors) && store_.change(s->source, p_value);
	}
	double v = p_value;
	if (s->row == OptionRowType::BarSelector && s->step > 0.0) {
		v = std::clamp(s->min + std::round((v - s->min) / s->step) * s->step, s->min, s->max);
	}
	return store_.change(s->source, v);
}

bool OptionsSession::change(std::string_view p_source, double p_value) {
	return store_.change(p_source, p_value);
}

bool OptionsSession::reset_to_default(std::string_view p_source) {
	const OptionSetting *s = store_.catalog() == nullptr ? nullptr : store_.catalog()->setting(p_source);
	if (s == nullptr) {
		return false;
	}
	if (s->kind == OptionKind::DisplayMode) {
		return store_.set_display_mode(uint32_t(store_.default_value(kResolutionWidth)),
				uint32_t(store_.default_value(kResolutionHeight)), uint32_t(store_.default_value(kResolutionHertz)));
	}
	return store_.set_value(s->option_id, store_.default_value(s->option_id));
}

bool OptionsSession::run(OptionsAction p_action) {
	switch (p_action) {
		case OptionsAction::Save:
			store_.save();
			return true;
		case OptionsAction::Revert:
			store_.revert();
			return true;
		case OptionsAction::ApplyVideo:
			store_.apply_video();
			return true;
		case OptionsAction::RevertVideo:
			store_.revert_video();
			return true;
		case OptionsAction::Reset:
			store_.reset_to_defaults();
			return true;
		case OptionsAction::ConfirmBind:
			return confirm_bind(true) == KeyBindResult::Bound;
		case OptionsAction::RevertBind:
			return confirm_bind(false) == KeyBindResult::Cancelled;
		case OptionsAction::BindKey:
		case OptionsAction::None:
			return false;
	}
	return false;
}

bool OptionsSession::bind_key(uint16_t p_row, KeyColumn p_column) {
	return capture_.begin(store_.key_bindings(), { key_tab_, p_row, p_column });
}

KeyBindResult OptionsSession::key_pressed(std::string_view p_input) {
	const KeyBindResult r = capture_.press(store_.key_bindings(), p_input);
	if (r == KeyBindResult::Bound) {
		store_.touch();
	}
	return r;
}

KeyBindResult OptionsSession::confirm_bind(bool p_bind_anyway) {
	const KeyBindResult r = capture_.confirm(store_.key_bindings(), p_bind_anyway);
	if (r == KeyBindResult::Bound) {
		store_.touch();
	}
	return r;
}

KeyBindResult OptionsSession::clear_bind() {
	const KeyBindResult r = capture_.clear(store_.key_bindings());
	if (r == KeyBindResult::Cleared) {
		store_.touch();
	}
	return r;
}

KeyBindResult OptionsSession::cancel_bind() {
	return capture_.cancel();
}

OptionRowData OptionsSession::row_data(const OptionSetting &p_setting) const {
	OptionRowData r;
	r.source = p_setting.source;
	r.field = snake(p_setting.source);
	r.data_key = p_setting.data_key;
	r.tab = p_setting.tab;
	r.list = p_setting.list;
	r.label = p_setting.label;
	r.hint = p_setting.label + "_HINT";
	r.row = p_setting.row;
	r.from = p_setting.min;
	r.to = p_setting.max;
	r.step = p_setting.step;
	r.value = store_.value(p_setting.option_id);
	switch (p_setting.kind) {
		case OptionKind::Bool:
		case OptionKind::Enum:
			for (size_t i = 0; i < p_setting.choices.size(); ++i) {
				r.choices.push_back({ p_setting.choices[i], int32_t(i) });
			}
			r.highlighted = int32_t(r.value);
			break;
		case OptionKind::Number:
			for (size_t i = 0; i < p_setting.choices.size(); ++i) {
				r.choices.push_back(
						{ p_setting.choices[i], int32_t(std::llround(p_setting.min + double(i) * p_setting.step)) });
			}
			r.highlighted =
					p_setting.step > 0.0 ? int32_t(std::llround((r.value - p_setting.min) / p_setting.step)) : 0;
			break;
		case OptionKind::DisplayMode: {
			const DisplayModeChoice now{ uint32_t(store_.value(kResolutionWidth)),
				uint32_t(store_.value(kResolutionHeight)), uint32_t(store_.value(kResolutionHertz)) };
			r.highlighted = 0;
			for (size_t i = 0; i < runtime_.display_modes.size(); ++i) {
				const DisplayModeChoice &m = runtime_.display_modes[i];
				r.choices.push_back({ display_mode_label(m), int32_t(i) });
				if (m == now) {
					r.highlighted = int32_t(i);
				}
			}
			break;
		}
		case OptionKind::Monitor:
			for (uint32_t i = 0; i < runtime_.monitors; ++i) {
				r.choices.push_back({ std::to_string(i + 1), int32_t(i) });
			}
			r.highlighted = int32_t(r.value);
			break;
	}
	return r;
}

OptionsModel OptionsSession::model() const {
	OptionsModel m;
	if (store_.catalog() != nullptr) {
		for (const OptionSetting &s : store_.catalog()->settings) {
			m.rows.push_back(row_data(s));
		}
	}
	for (const KeyBindingTab &t : store_.key_bindings().tabs()) {
		m.key_tab_labels.push_back(t.label);
		std::vector<KeyBindingRowData> rows;
		for (const KeyBindingRow &r : t.rows) {
			KeyBindingRowData d;
			d.label = r.label;
			for (size_t c = 0; c < kKeyColumnCount; ++c) {
				d.inputs[c] = key_input_sid(r.inputs[c]);
			}
			rows.push_back(std::move(d));
		}
		m.key_bindings.push_back(std::move(rows));
	}
	m.selected_tab = selected_tab_;
	m.key_tab = key_tab_;
	const bool capturing = capture_.waiting() || capture_.conflict_pending();
	m.key_binding_mode = capture_.waiting();
	m.key_bind_conflict = capture_.conflict_pending();
	m.key_bindings_row = capturing ? int32_t(capture_.cell().row) : -1;
	m.key_bindings_column = capturing ? int32_t(capture_.cell().column) : -1;
	m.settings_changed = store_.settings_changed();
	m.video_settings_changed = store_.video_settings_changed();
	m.exiting_options = exiting_;
	return m;
}

}
