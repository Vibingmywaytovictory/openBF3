#pragma once

#include "bf3/core/ui/key_bindings.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/ui/settings_store.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

struct DisplayModeChoice {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t hertz = 0;
	bool operator==(const DisplayModeChoice &) const = default;
};

struct OptionsRuntime {
	std::vector<DisplayModeChoice> display_modes;
	uint32_t monitors = 1;
};

std::string display_mode_label(const DisplayModeChoice &p_mode);

struct OptionChoice {
	std::string label;
	int32_t index = 0;
	bool operator==(const OptionChoice &) const = default;
};

struct OptionRowData {
	std::string source;
	std::string field;
	int32_t data_key = 0;
	OptionsTab tab = OptionsTab::Controls;
	std::string list;
	std::string label;
	std::string hint;
	OptionRowType row = OptionRowType::TextSelector;
	std::vector<OptionChoice> choices;
	int32_t highlighted = 0;
	double from = 0.0;
	double to = 0.0;
	double step = 0.0;
	double value = 0.0;
};

struct KeyBindingRowData {
	std::string label;
	std::array<std::string, kKeyColumnCount> inputs;
};

struct OptionsModel {
	std::vector<OptionRowData> rows;
	std::vector<std::string> key_tab_labels;
	std::vector<std::vector<KeyBindingRowData>> key_bindings;
	uint8_t selected_tab = 0;
	uint8_t key_tab = 0;
	uint8_t key_column = 0;
	int32_t key_bindings_row = -1;
	int32_t key_bindings_column = -1;
	bool key_binding_mode = false;
	bool key_bind_conflict = false;
	bool settings_changed = false;
	bool video_settings_changed = false;
	bool exiting_options = false;

	const OptionRowData *row_of_key(int32_t p_data_key) const;
	const OptionRowData *row_of_field(std::string_view p_field) const;
	bool has_field(std::string_view p_field) const;
};

inline constexpr std::string_view kOptionsStateFields[] = {
	"settings_changed",
	"video_settings_changed",
	"exiting_options",
	"selected_tab",
	"key_bindings",
	"key_bindings_column",
	"key_bindings_row",
	"key_binding_mode",
};

enum class OptionsAction : uint8_t {
	None,
	Save,
	Revert,
	ApplyVideo,
	RevertVideo,
	Reset,
	BindKey,
	ConfirmBind,
	RevertBind
};

OptionsAction options_action_of(int32_t p_action_key, std::span<const std::string> p_params);

class OptionsSession {
public:
	OptionsSession() = default;
	OptionsSession(const OptionsCatalog &p_catalog, KeyBindingTable p_bindings);

	SettingsStore &store() { return store_; }
	const SettingsStore &store() const { return store_; }
	OptionsRuntime &runtime() { return runtime_; }
	const OptionsRuntime &runtime() const { return runtime_; }
	const KeyBindCapture &capture() const { return capture_; }

	void enter();
	void exit();
	bool exiting() const { return exiting_; }
	void select_tab(uint8_t p_tab);
	void select_key_tab(uint8_t p_tab);
	void select_key_column(uint8_t p_column);

	bool set_option_value(int32_t p_data_key, double p_value);
	bool change(std::string_view p_source, double p_value);
	bool reset_to_default(std::string_view p_source);
	bool run(OptionsAction p_action);

	bool bind_key(uint16_t p_row, KeyColumn p_column);
	KeyBindResult key_pressed(std::string_view p_input);
	KeyBindResult confirm_bind(bool p_bind_anyway);
	KeyBindResult clear_bind();
	KeyBindResult cancel_bind();

	OptionsModel model() const;

private:
	OptionRowData row_data(const OptionSetting &p_setting) const;

	SettingsStore store_;
	OptionsRuntime runtime_;
	KeyBindCapture capture_;
	uint8_t selected_tab_ = 0;
	uint8_t key_tab_ = 0;
	uint8_t key_column_ = 0;
	bool exiting_ = false;
};

}
