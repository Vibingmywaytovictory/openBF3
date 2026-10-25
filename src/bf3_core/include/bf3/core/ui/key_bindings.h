#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/ui_input.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::ui {

inline constexpr std::string_view kInputConfigurationPartition = "input/inputconfiguration";
inline constexpr std::string_view kKeyBindingScreen = "ui/flow/screen/optionskeybindingscreen";
inline constexpr std::string_view kKeyBindingTabBar = "TabBar_02";
inline constexpr size_t kMaxInputActionMaps = 16;
inline constexpr size_t kMaxInputConcepts = 128;
inline constexpr size_t kMaxConceptInputs = 8;

enum class ConceptDevice : uint8_t { Keyboard, Mouse, Pad };

struct ConceptInput {
	ConceptDevice device = ConceptDevice::Keyboard;
	std::string input;
	bool negate = false;
	bool analog = false;
	bool operator==(const ConceptInput &) const = default;
};

struct InputConcept {
	std::string id;
	std::string name_sid;
	bool hidden = false;
	std::string copy_from;
	std::vector<ConceptInput> inputs;
};

struct InputActionMap {
	std::string id;
	std::string name_id;
	std::string partition;
	int32_t settings_scheme = 0;
	std::vector<InputConcept> concepts;

	const InputConcept *find(std::string_view p_concept) const;
};

struct KeyBindingMaps {
	std::vector<InputActionMap> maps;

	const InputActionMap *find(std::string_view p_id) const;
};

Result<KeyBindingMaps> read_key_binding_maps(const EbxResolver &p_db);
Result<std::vector<std::string>> read_key_binding_tabs(const EbxResolver &p_db);

inline constexpr std::array<std::string_view, 5> kKeyBindingTabMaps = { "infantry", "vehicle", "jet", "heli",
	"default" };

enum class KeyColumn : uint8_t { Keyboard, Mouse, Joystick };
inline constexpr size_t kKeyColumnCount = 3;
std::string_view key_column_name(KeyColumn p_column);

bool input_fits(KeyColumn p_column, std::string_view p_input);
std::string key_input_sid(std::string_view p_input);

struct KeyBindingRow {
	std::string concept_id;
	std::string label;
	bool negative = false;
	std::array<std::string, kKeyColumnCount> defaults;
	std::array<std::string, kKeyColumnCount> inputs;
};

struct KeyBindingTab {
	std::string label;
	std::string map_id;
	std::vector<KeyBindingRow> rows;
};

std::vector<KeyBindingRow> key_binding_rows(const InputActionMap &p_map);

struct KeyCell {
	uint8_t tab = 0;
	uint16_t row = 0;
	KeyColumn column = KeyColumn::Keyboard;
	bool operator==(const KeyCell &) const = default;
};

class KeyBindingTable {
public:
	static Result<KeyBindingTable> build(const KeyBindingMaps &p_maps, std::span<const std::string> p_tab_labels);

	const std::vector<KeyBindingTab> &tabs() const { return tabs_; }
	const KeyBindingTab *tab_of_map(std::string_view p_map) const;
	const KeyBindingRow *row(std::string_view p_map, std::string_view p_concept, bool p_negative) const;
	bool valid(KeyCell p_cell) const;
	const std::string &input(KeyCell p_cell) const;
	bool set(KeyCell p_cell, std::string_view p_input);
	std::vector<KeyCell> holders(uint8_t p_tab, KeyColumn p_column, std::string_view p_input) const;
	void reset();
	bool is_default() const;

	std::vector<std::pair<std::string, std::string>> overrides() const;
	bool apply_override(std::string_view p_key, std::string_view p_value);

private:
	std::vector<KeyBindingTab> tabs_;
};

std::string key_override_key(std::string_view p_map, const KeyBindingRow &p_row, KeyColumn p_column);

UiInputBindings ui_input_bindings_with(const UiInputBindings &p_base, const KeyBindingTable &p_table);

enum class KeyBindResult : uint8_t { Bound, Conflict, Cancelled, Cleared, Refused };

class KeyBindCapture {
public:
	bool begin(const KeyBindingTable &p_table, KeyCell p_cell);
	bool waiting() const { return waiting_; }
	bool conflict_pending() const { return conflict_; }
	KeyCell cell() const { return cell_; }
	const std::string &pending_input() const { return pending_; }
	const std::vector<KeyCell> &conflicts() const { return conflicts_; }

	KeyBindResult press(KeyBindingTable &r_table, std::string_view p_input);
	KeyBindResult confirm(KeyBindingTable &r_table, bool p_bind_anyway);
	KeyBindResult cancel();
	KeyBindResult clear(KeyBindingTable &r_table);

private:
	KeyCell cell_;
	std::string pending_;
	std::vector<KeyCell> conflicts_;
	bool waiting_ = false;
	bool conflict_ = false;
};

}
