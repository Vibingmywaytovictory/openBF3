#include "bf3/core/ui/key_bindings.h"

#include "ui_ebx.h"

#include "bf3/core/util/text.h"

#include <algorithm>

namespace bf3::core::ui {

namespace {

constexpr std::array<std::string_view, kKeyColumnCount> kColumnNames = { "keyboard", "mouse", "joystick" };
constexpr std::string_view kEscape = "IDK_Escape";
constexpr std::string_view kOverridePrefix = "bind.";

using detail::text_of;

std::optional<ConceptInput> input_of(const EbxInstance &p_i) {
	ConceptInput in;
	in.negate = detail::flag_or(p_i.data(), "NegateValue", false);
	in.analog = detail::flag_or(p_i.data(), "IsAnalog", false);
	if (p_i.is_a("KeyboardInputActionData")) {
		in.device = ConceptDevice::Keyboard;
		in.input = text_of(p_i.get("Key"));
		return in;
	}
	if (p_i.is_a("MouseInputActionData")) {
		in.device = ConceptDevice::Mouse;
	} else if (p_i.is_a("PadInputActionData")) {
		in.device = ConceptDevice::Pad;
	} else {
		return std::nullopt;
	}
	const std::string axis = text_of(p_i.get("Axis"));
	in.input = input_is_bound(axis) ? axis : text_of(p_i.get("Button"));
	return in;
}

Result<InputActionMap> map_of(const EbxResolver &p_db, const EbxPartition &p_part, std::string_view p_partition) {
	const EbxInstance root = p_part.primary();
	if (!root.is_a("EntryInputActionMapsData")) {
		return make_error("key bindings: " + std::string(p_partition) + " is a " + std::string(root.type_name()));
	}
	InputActionMap out;
	out.partition = std::string(p_partition);
	out.settings_scheme = detail::i32_or(root.data(), "ActionMapSettingsScheme", 0);
	for (const EbxInstance &m : p_db.follow_all(root.data(), "InputActionMaps")) {
		const std::string platform = text_of(m.get("PlatformSpecific"));
		if (platform != "IAMPWin32" && platform != "IAMPAllPlatforms") {
			continue;
		}
		for (const EbxInstance &a : p_db.follow_all(m.data(), "Actions")) {
			if (out.concepts.size() >= kMaxInputConcepts) {
				return make_error("key bindings: " + std::string(p_partition) + " has more than " +
						std::to_string(kMaxInputConcepts) + " concepts");
			}
			InputConcept c;
			c.id = text_of(a.get("ConceptIdentifier"));
			c.name_sid = text_of(a.get("NameSid"));
			c.hidden = detail::flag_or(a.data(), "HideInKeyBindings", false);
			c.copy_from = text_of(a.get("CopyKeyBindingFrom"));
			for (const EbxInstance &i : p_db.follow_all(a.data(), "InputActions")) {
				if (c.inputs.size() >= kMaxConceptInputs) {
					return make_error("key bindings: " + c.id + " has more than " + std::to_string(kMaxConceptInputs) +
							" inputs");
				}
				if (std::optional<ConceptInput> in = input_of(i)) {
					c.inputs.push_back(std::move(*in));
				}
			}
			out.concepts.push_back(std::move(c));
		}
		return out;
	}
	return make_error("key bindings: " + std::string(p_partition) + " has no Win32 map");
}

bool split_override(std::string_view p_key, std::string_view &r_map, std::string_view &r_concept, bool &r_negative,
		std::string_view &r_column) {
	if (!p_key.starts_with(kOverridePrefix)) {
		return false;
	}
	std::string_view rest = p_key.substr(kOverridePrefix.size());
	std::vector<std::string_view> parts;
	while (!rest.empty()) {
		const size_t dot = rest.find('.');
		parts.push_back(rest.substr(0, dot));
		rest = dot == std::string_view::npos ? std::string_view() : rest.substr(dot + 1);
	}
	if (parts.size() == 3) {
		r_negative = false;
	} else if (parts.size() == 4 && parts[2] == "neg") {
		r_negative = true;
	} else {
		return false;
	}
	r_map = parts[0];
	r_concept = parts[1];
	r_column = parts.back();
	return true;
}

}

const InputConcept *InputActionMap::find(std::string_view p_concept) const {
	const auto it =
			std::find_if(concepts.begin(), concepts.end(), [&](const InputConcept &c) { return c.id == p_concept; });
	return it == concepts.end() ? nullptr : &*it;
}

const InputActionMap *KeyBindingMaps::find(std::string_view p_id) const {
	const auto it = std::find_if(maps.begin(), maps.end(), [&](const InputActionMap &m) { return m.id == p_id; });
	return it == maps.end() ? nullptr : &*it;
}

Result<KeyBindingMaps> read_key_binding_maps(const EbxResolver &p_db) {
	const EbxPartition *part = p_db.partition_by_name(kInputConfigurationPartition);
	if (part == nullptr) {
		return make_error("key bindings: no partition " + std::string(kInputConfigurationPartition));
	}
	const EbxInstance root = part->primary();
	if (!root.is_a("InputConfigurationAsset")) {
		return make_error("key bindings: the input configuration is a " + std::string(root.type_name()));
	}
	const std::optional<EbxValue> list = root.get("UserConfigurableActionMaps");
	const OptionalEbxArray arr = list ? list->as_array() : OptionalEbxArray();
	if (!arr || arr->size() > kMaxInputActionMaps) {
		return make_error(
				"key bindings: no UserConfigurableActionMaps or more than " + std::to_string(kMaxInputActionMaps));
	}
	KeyBindingMaps out;
	for (const EbxValue &v : *arr) {
		const std::optional<EbxStruct> s = v.as_struct();
		if (!s) {
			continue;
		}
		const std::string id = text_of(s->get("Id"));
		const std::optional<EbxRef> ref = detail::ref(*s, "ActionMap");
		const std::optional<std::string> name = ref ? detail::ref_partition(p_db, *part, *ref) : std::nullopt;
		const EbxPartition *map_part = name && !name->empty() ? p_db.partition_by_name(*name) : nullptr;
		if (map_part == nullptr) {
			return make_error("key bindings: map " + id + " has no partition");
		}
		Result<InputActionMap> m = map_of(p_db, *map_part, *name);
		if (!m) {
			return m.error();
		}
		m->id = id;
		m->name_id = text_of(s->get("NameId"));
		out.maps.push_back(std::move(*m));
	}
	return out;
}

Result<std::vector<std::string>> read_key_binding_tabs(const EbxResolver &p_db) {
	const EbxPartition *part = p_db.partition_by_name(kKeyBindingScreen);
	if (part == nullptr) {
		return make_error("key bindings: no partition " + std::string(kKeyBindingScreen));
	}
	for (const EbxInstance &n : p_db.follow_all(part->primary().data(), "Nodes")) {
		if (!n.is_a("WidgetNode") || text_of(n.get("InstanceName")) != kKeyBindingTabBar) {
			continue;
		}
		const std::optional<EbxValue> props = n.get("WidgetProperties");
		const OptionalEbxArray arr = props ? props->as_array() : OptionalEbxArray();
		if (!arr) {
			break;
		}
		for (const EbxValue &v : *arr) {
			const std::optional<EbxStruct> s = v.as_struct();
			if (!s || text_of(s->get("Name")) != "Tabs") {
				continue;
			}
			std::vector<std::string> out;
			const std::string tabs = text_of(s->get("Value"));
			size_t at = 0;
			while (at < tabs.size()) {
				const size_t semi = std::min(tabs.find(';', at), tabs.size());
				out.push_back(tabs.substr(at, semi - at));
				at = semi + 1;
			}
			return out;
		}
	}
	return make_error("key bindings: no Tabs on " + std::string(kKeyBindingTabBar));
}

std::string_view key_column_name(KeyColumn p_column) {
	return size_t(p_column) < kColumnNames.size() ? kColumnNames[size_t(p_column)] : std::string_view("?");
}

bool input_fits(KeyColumn p_column, std::string_view p_input) {
	if (!input_is_bound(p_input)) {
		return false;
	}
	switch (p_column) {
		case KeyColumn::Keyboard:
			return p_input.starts_with("IDK_");
		case KeyColumn::Mouse:
			return p_input.starts_with("IDB_Button_") || p_input.starts_with("IDA_");
		case KeyColumn::Joystick:
			return (p_input.starts_with("IDB_") && !p_input.starts_with("IDB_Button_")) || p_input.starts_with("IDA_");
	}
	return false;
}

std::string key_input_sid(std::string_view p_input) {
	return input_is_bound(p_input) ? "ID_" + std::string(p_input) : std::string();
}

std::vector<KeyBindingRow> key_binding_rows(const InputActionMap &p_map) {
	std::vector<KeyBindingRow> out;
	for (const InputConcept &c : p_map.concepts) {
		if (c.hidden) {
			continue;
		}
		const std::string label = c.name_sid.empty() ? "ID_M_OPTIONS_" + to_upper_ascii(c.id) : c.name_sid;
		const bool axis = std::any_of(c.inputs.begin(), c.inputs.end(), [](const ConceptInput &i) {
			return (i.input.starts_with("IDA_") && input_is_bound(i.input)) ||
					(i.device == ConceptDevice::Keyboard && i.negate);
		});
		for (const bool negative : { false, true }) {
			if (negative && !axis) {
				break;
			}
			KeyBindingRow r;
			r.concept_id = c.id;
			r.label = negative ? label + "_NEG" : label;
			r.negative = negative;
			std::array<bool, kKeyColumnCount> filled{};
			for (const ConceptInput &i : c.inputs) {
				const size_t col = i.device == ConceptDevice::Keyboard ? 0 : i.device == ConceptDevice::Mouse ? 1 : 2;
				const bool belongs = i.input.starts_with("IDA_") || i.negate == negative;
				if (!belongs || filled[col]) {
					continue;
				}
				filled[col] = true;
				r.defaults[col] = input_is_bound(i.input) ? i.input : std::string();
			}
			r.inputs = r.defaults;
			out.push_back(std::move(r));
		}
	}
	return out;
}

Result<KeyBindingTable> KeyBindingTable::build(
		const KeyBindingMaps &p_maps, std::span<const std::string> p_tab_labels) {
	if (p_tab_labels.size() != kKeyBindingTabMaps.size()) {
		return make_error("key bindings: " + std::to_string(p_tab_labels.size()) + " tabs, not " +
				std::to_string(kKeyBindingTabMaps.size()));
	}
	KeyBindingTable out;
	for (size_t t = 0; t < kKeyBindingTabMaps.size(); ++t) {
		const InputActionMap *m = p_maps.find(kKeyBindingTabMaps[t]);
		if (m == nullptr) {
			return make_error("key bindings: no map " + std::string(kKeyBindingTabMaps[t]));
		}
		KeyBindingTab tab;
		tab.label = p_tab_labels[t];
		tab.map_id = m->id;
		tab.rows = key_binding_rows(*m);
		for (size_t i = 0; i < tab.rows.size(); ++i) {
			for (size_t j = 0; j < i; ++j) {
				if (tab.rows[i].concept_id == tab.rows[j].concept_id && tab.rows[i].negative == tab.rows[j].negative) {
					return make_error("key bindings: " + m->id + " shows " + tab.rows[i].concept_id + " twice");
				}
			}
		}
		out.tabs_.push_back(std::move(tab));
	}
	return out;
}

const KeyBindingTab *KeyBindingTable::tab_of_map(std::string_view p_map) const {
	const auto it = std::find_if(tabs_.begin(), tabs_.end(), [&](const KeyBindingTab &t) { return t.map_id == p_map; });
	return it == tabs_.end() ? nullptr : &*it;
}

const KeyBindingRow *KeyBindingTable::row(std::string_view p_map, std::string_view p_concept, bool p_negative) const {
	const KeyBindingTab *t = tab_of_map(p_map);
	if (t == nullptr) {
		return nullptr;
	}
	const auto it = std::find_if(t->rows.begin(), t->rows.end(),
			[&](const KeyBindingRow &r) { return r.concept_id == p_concept && r.negative == p_negative; });
	return it == t->rows.end() ? nullptr : &*it;
}

bool KeyBindingTable::valid(KeyCell p_cell) const {
	return p_cell.tab < tabs_.size() && p_cell.row < tabs_[p_cell.tab].rows.size() &&
			size_t(p_cell.column) < kKeyColumnCount;
}

const std::string &KeyBindingTable::input(KeyCell p_cell) const {
	static const std::string kEmpty;
	return valid(p_cell) ? tabs_[p_cell.tab].rows[p_cell.row].inputs[size_t(p_cell.column)] : kEmpty;
}

bool KeyBindingTable::set(KeyCell p_cell, std::string_view p_input) {
	if (!valid(p_cell) || (!p_input.empty() && !input_fits(p_cell.column, p_input))) {
		return false;
	}
	tabs_[p_cell.tab].rows[p_cell.row].inputs[size_t(p_cell.column)] = std::string(p_input);
	return true;
}

std::vector<KeyCell> KeyBindingTable::holders(uint8_t p_tab, KeyColumn p_column, std::string_view p_input) const {
	std::vector<KeyCell> out;
	if (p_tab >= tabs_.size() || !input_is_bound(p_input)) {
		return out;
	}
	const std::vector<KeyBindingRow> &rows = tabs_[p_tab].rows;
	for (size_t r = 0; r < rows.size(); ++r) {
		if (rows[r].inputs[size_t(p_column)] == p_input) {
			out.push_back({ p_tab, uint16_t(r), p_column });
		}
	}
	return out;
}

void KeyBindingTable::reset() {
	for (KeyBindingTab &t : tabs_) {
		for (KeyBindingRow &r : t.rows) {
			r.inputs = r.defaults;
		}
	}
}

bool KeyBindingTable::is_default() const {
	return std::all_of(tabs_.begin(), tabs_.end(), [](const KeyBindingTab &t) {
		return std::all_of(t.rows.begin(), t.rows.end(), [](const KeyBindingRow &r) { return r.inputs == r.defaults; });
	});
}

std::string key_override_key(std::string_view p_map, const KeyBindingRow &p_row, KeyColumn p_column) {
	return std::string(kOverridePrefix) + std::string(p_map) + "." + p_row.concept_id +
			(p_row.negative ? ".neg." : ".") + std::string(key_column_name(p_column));
}

std::vector<std::pair<std::string, std::string>> KeyBindingTable::overrides() const {
	std::vector<std::pair<std::string, std::string>> out;
	for (const KeyBindingTab &t : tabs_) {
		for (const KeyBindingRow &r : t.rows) {
			for (size_t c = 0; c < kKeyColumnCount; ++c) {
				if (r.inputs[c] != r.defaults[c]) {
					out.emplace_back(key_override_key(t.map_id, r, KeyColumn(c)), r.inputs[c]);
				}
			}
		}
	}
	return out;
}

bool KeyBindingTable::apply_override(std::string_view p_key, std::string_view p_value) {
	std::string_view map;
	std::string_view concept_id;
	std::string_view column;
	bool negative = false;
	if (!split_override(p_key, map, concept_id, negative, column)) {
		return false;
	}
	const auto col = std::find(kColumnNames.begin(), kColumnNames.end(), column);
	if (col == kColumnNames.end()) {
		return false;
	}
	for (size_t t = 0; t < tabs_.size(); ++t) {
		if (tabs_[t].map_id != map) {
			continue;
		}
		for (size_t r = 0; r < tabs_[t].rows.size(); ++r) {
			const KeyBindingRow &row = tabs_[t].rows[r];
			if (row.concept_id == concept_id && row.negative == negative) {
				return set({ uint8_t(t), uint16_t(r), KeyColumn(col - kColumnNames.begin()) }, p_value);
			}
		}
	}
	return false;
}

UiInputBindings ui_input_bindings_with(const UiInputBindings &p_base, const KeyBindingTable &p_table) {
	UiInputBindings out = p_base;
	const KeyBindingTab *t = p_table.tab_of_map("default");
	if (t == nullptr) {
		return out;
	}
	for (const KeyBindingRow &r : t->rows) {
		if (r.negative) {
			continue;
		}
		for (const KeyColumn c : { KeyColumn::Keyboard, KeyColumn::Mouse }) {
			const std::string &now = r.inputs[size_t(c)];
			const std::string &was = r.defaults[size_t(c)];
			if (now == was) {
				continue;
			}
			const UiInputDevice device = c == KeyColumn::Keyboard ? UiInputDevice::Keyboard : UiInputDevice::Mouse;
			std::erase_if(out.bindings, [&](const UiKeyBinding &b) {
				return b.ui_concept == r.concept_id && b.device == device && b.key == was;
			});
			if (input_is_bound(now) && (c == KeyColumn::Keyboard || now.starts_with("IDB_"))) {
				out.bindings.push_back({ r.concept_id, device, now });
			}
		}
	}
	return out;
}

bool KeyBindCapture::begin(const KeyBindingTable &p_table, KeyCell p_cell) {
	if (!p_table.valid(p_cell)) {
		return false;
	}
	cell_ = p_cell;
	pending_.clear();
	conflicts_.clear();
	waiting_ = true;
	conflict_ = false;
	return true;
}

KeyBindResult KeyBindCapture::press(KeyBindingTable &r_table, std::string_view p_input) {
	if (!waiting_ || conflict_) {
		return KeyBindResult::Refused;
	}
	if (p_input == kEscape) {
		return cancel();
	}
	if (!input_fits(cell_.column, p_input)) {
		return KeyBindResult::Refused;
	}
	conflicts_.clear();
	for (const KeyCell &h : r_table.holders(cell_.tab, cell_.column, p_input)) {
		if (!(h == cell_)) {
			conflicts_.push_back(h);
		}
	}
	if (!conflicts_.empty()) {
		pending_ = std::string(p_input);
		conflict_ = true;
		return KeyBindResult::Conflict;
	}
	r_table.set(cell_, p_input);
	waiting_ = false;
	return KeyBindResult::Bound;
}

KeyBindResult KeyBindCapture::confirm(KeyBindingTable &r_table, bool p_bind_anyway) {
	if (!conflict_) {
		return KeyBindResult::Refused;
	}
	conflict_ = false;
	waiting_ = false;
	if (!p_bind_anyway) {
		pending_.clear();
		return KeyBindResult::Cancelled;
	}
	r_table.set(cell_, pending_);
	return KeyBindResult::Bound;
}

KeyBindResult KeyBindCapture::cancel() {
	const bool was = waiting_ || conflict_;
	waiting_ = false;
	conflict_ = false;
	pending_.clear();
	return was ? KeyBindResult::Cancelled : KeyBindResult::Refused;
}

KeyBindResult KeyBindCapture::clear(KeyBindingTable &r_table) {
	if (!waiting_ || conflict_) {
		return KeyBindResult::Refused;
	}
	r_table.set(cell_, "");
	waiting_ = false;
	return KeyBindResult::Cleared;
}

}
