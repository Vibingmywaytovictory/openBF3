#include "bf3/core/ui/ui_input.h"

#include "ui_ebx.h"

#include "bf3/core/ebx.h"

#include <algorithm>
#include <array>

namespace bf3::core::ui {

namespace {

struct ConceptRow {
	UiInputConcept ui_concept;
	std::string_view name;
	std::string_view identifier;
};

constexpr std::array<ConceptRow, kUiInputConceptCount> kConcepts = { {
		{ UiInputConcept::NavigateUp, "NavigateUp", "ConceptMoveForward" },
		{ UiInputConcept::NavigateDown, "NavigateDown", "ConceptMoveBackward" },
		{ UiInputConcept::NavigateLeft, "NavigateLeft", "ConceptMoveLeft" },
		{ UiInputConcept::NavigateRight, "NavigateRight", "ConceptMoveRight" },
		{ UiInputConcept::TabLeft, "TabLeft", "ConceptParentNavigateLeft" },
		{ UiInputConcept::TabRight, "TabRight", "ConceptParentNavigateRight" },
		{ UiInputConcept::Activate, "Activate", "ConceptActivate" },
		{ UiInputConcept::Deactivate, "Deactivate", "ConceptDeactivate" },
		{ UiInputConcept::Menu, "Menu", "ConceptMenu" },
		{ UiInputConcept::Cancel, "Cancel", "ConceptCancel" },
		{ UiInputConcept::OK, "OK", "" },
		{ UiInputConcept::Back, "Back", "ConceptBack" },
		{ UiInputConcept::Tab, "Tab", "ConceptScoreboard" },
		{ UiInputConcept::Edit, "Edit", "ConceptEdit" },
		{ UiInputConcept::View, "View", "ConceptView" },
		{ UiInputConcept::LThumb, "LThumb", "ConceptLThumb" },
		{ UiInputConcept::RThumb, "RThumb", "ConceptRThumb" },
		{ UiInputConcept::MapZoom, "MapZoom", "ConceptMapZoom" },
		{ UiInputConcept::MapSize, "MapSize", "ConceptMapSize" },
		{ UiInputConcept::SayAllChat, "SayAllChat", "ConceptSayAllChat" },
		{ UiInputConcept::TeamChat, "TeamChat", "ConceptTeamChat" },
		{ UiInputConcept::SquadChat, "SquadChat", "ConceptSquadChat" },
		{ UiInputConcept::Com, "Com", "ConceptCommMenu1" },
} };

struct KeyRow {
	std::string_view idk;
	int32_t code;
};

constexpr KeyRow kKeys[] = {
	{ "IDK_Backspace", 8 },
	{ "IDK_Tab", 9 },
	{ "IDK_Enter", 13 },
	{ "IDK_NumpadEnter", 13 },
	{ "IDK_LeftShift", 16 },
	{ "IDK_RightShift", 16 },
	{ "IDK_LeftCtrl", 17 },
	{ "IDK_RightCtrl", 17 },
	{ "IDK_LeftAlt", 18 },
	{ "IDK_RightAlt", 18 },
	{ "IDK_Pause", 19 },
	{ "IDK_CapsLock", 20 },
	{ "IDK_Escape", 27 },
	{ "IDK_Space", 32 },
	{ "IDK_PageUp", 33 },
	{ "IDK_PageDown", 34 },
	{ "IDK_End", 35 },
	{ "IDK_Home", 36 },
	{ "IDK_ArrowLeft", 37 },
	{ "IDK_ArrowUp", 38 },
	{ "IDK_ArrowRight", 39 },
	{ "IDK_ArrowDown", 40 },
	{ "IDK_Insert", 45 },
	{ "IDK_Delete", 46 },
	{ "IDK_Multiply", 106 },
	{ "IDK_Add", 107 },
	{ "IDK_Subtract", 109 },
	{ "IDK_Decimal", 110 },
	{ "IDK_Divide", 111 },
	{ "IDK_Semicolon", 186 },
	{ "IDK_Equals", 187 },
	{ "IDK_Comma", 188 },
	{ "IDK_Minus", 189 },
	{ "IDK_Period", 190 },
	{ "IDK_Slash", 191 },
	{ "IDK_Grave", 192 },
	{ "IDK_LeftBracket", 219 },
	{ "IDK_Backslash", 220 },
	{ "IDK_RightBracket", 221 },
	{ "IDK_Apostrophe", 222 },
};

constexpr std::string_view kUiInputActionPrefix = "UIInputAction_";
constexpr std::string_view kIdkPrefix = "IDK_";
}

bool input_is_bound(std::string_view p_input) {
	return !p_input.empty() && !p_input.ends_with("_Undefined") && p_input != "IDK_None";
}

std::string_view ui_input_concept_name(UiInputConcept p_concept) {
	const int32_t i = int32_t(p_concept);
	return i >= 0 && i < kUiInputConceptCount ? kConcepts[size_t(i)].name : std::string_view();
}

std::string_view ui_input_concept_identifier(UiInputConcept p_concept) {
	const int32_t i = int32_t(p_concept);
	return i >= 0 && i < kUiInputConceptCount ? kConcepts[size_t(i)].identifier : std::string_view();
}

std::optional<UiInputConcept> ui_input_concept_of_action(std::string_view p_ui_input_action) {
	if (!p_ui_input_action.starts_with(kUiInputActionPrefix)) {
		return std::nullopt;
	}
	const std::string_view name = p_ui_input_action.substr(kUiInputActionPrefix.size());
	for (const ConceptRow &r : kConcepts) {
		if (r.name == name) {
			return r.ui_concept;
		}
	}
	return std::nullopt;
}

int32_t ui_flash_key_code(std::string_view p_idk) {
	if (!p_idk.starts_with(kIdkPrefix)) {
		return 0;
	}
	const std::string_view rest = p_idk.substr(kIdkPrefix.size());
	if (rest.size() == 1 && rest[0] >= 'A' && rest[0] <= 'Z') {
		return int32_t(rest[0]);
	}
	if (rest.size() == 1 && rest[0] >= '0' && rest[0] <= '9') {
		return int32_t(rest[0]);
	}
	if (rest.size() >= 2 && rest.size() <= 3 && rest[0] == 'F') {
		int32_t n = 0;
		for (size_t i = 1; i < rest.size(); ++i) {
			if (rest[i] < '0' || rest[i] > '9') {
				return 0;
			}
			n = n * 10 + (rest[i] - '0');
		}
		return n >= 1 && n <= 15 ? 111 + n : 0;
	}
	if (rest.starts_with("Numpad") && rest.size() == 7 && rest[6] >= '0' && rest[6] <= '9') {
		return 96 + (rest[6] - '0');
	}
	for (const KeyRow &k : kKeys) {
		if (k.idk == p_idk) {
			return k.code;
		}
	}
	return 0;
}

std::vector<UiInputConcept> UiInputBindings::concepts_of(UiInputDevice p_device, std::string_view p_key) const {
	std::vector<UiInputConcept> out;
	for (const UiKeyBinding &b : bindings) {
		if (b.device != p_device || b.key != p_key) {
			continue;
		}
		for (const ConceptRow &r : kConcepts) {
			if (!r.identifier.empty() && r.identifier == b.ui_concept &&
					std::find(out.begin(), out.end(), r.ui_concept) == out.end()) {
				out.push_back(r.ui_concept);
			}
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::vector<std::string> UiInputBindings::keys_of(std::string_view p_concept) const {
	std::vector<std::string> out;
	for (const UiKeyBinding &b : bindings) {
		if (b.ui_concept == p_concept) {
			out.push_back(b.key);
		}
	}
	return out;
}

Result<UiInputBindings> read_ui_input_bindings(const EbxResolver &p_db, std::string_view p_partition) {
	const EbxPartition *part = p_db.partition_by_name(p_partition);
	if (part == nullptr) {
		return make_error("ui input: no partition " + std::string(p_partition));
	}
	const EbxInstance root = part->primary();
	if (!root.is_a("EntryInputActionMapsData")) {
		return make_error("ui input: " + std::string(p_partition) + " is a " + std::string(root.type_name()) +
				", not an EntryInputActionMapsData");
	}
	UiInputBindings out;
	const auto each_ref = [&](const EbxInstance &p_i, std::string_view p_field, const auto &p_fn) {
		const std::optional<EbxValue> v = p_i.get(p_field);
		const OptionalEbxArray arr = v ? v->as_array() : OptionalEbxArray();
		if (!arr) {
			return;
		}
		for (const EbxValue &e : *arr) {
			const std::optional<EbxRef> r = e.as_ref();
			if (!r || r->kind != EbxRef::Kind::Internal) {
				continue;
			}
			if (const std::optional<EbxInstance> t = part->resolve(*r)) {
				p_fn(*t);
			}
		}
	};
	bool full = false;
	each_ref(root, "InputActionMaps", [&](const EbxInstance &p_map) {
		each_ref(p_map, "Actions", [&](const EbxInstance &p_actions) {
			const std::optional<EbxValue> id = p_actions.get("ConceptIdentifier");
			const std::optional<std::string_view> ui_concept = !id ? std::nullopt
					: id->enum_name()							   ? id->enum_name()
																   : id->as_str();
			if (!ui_concept) {
				return;
			}
			each_ref(p_actions, "InputActions", [&](const EbxInstance &p_action) {
				UiKeyBinding b;
				b.ui_concept = std::string(*ui_concept);
				std::optional<EbxValue> key;
				if (p_action.is_a("KeyboardInputActionData")) {
					key = p_action.get("Key");
				} else if (p_action.is_a("MouseInputActionData")) {
					b.device = UiInputDevice::Mouse;
					key = p_action.get("Button");
				} else {
					return;
				}
				const std::optional<std::string_view> k = key ? key->enum_name() : std::nullopt;
				if (!k || !input_is_bound(*k)) {
					return;
				}
				if (out.bindings.size() >= kUiInputMaxBindings) {
					full = true;
					return;
				}
				b.key = std::string(*k);
				out.bindings.push_back(std::move(b));
			});
		});
	});
	if (full) {
		return make_error("ui input: " + std::string(p_partition) + " has more than " +
				std::to_string(kUiInputMaxBindings) + " bindings");
	}
	return out;
}

UiInputRouter::UiInputRouter(UiInputBindings p_bindings) : bindings_(std::move(p_bindings)) {}

void UiInputRouter::set_bindings(UiInputBindings p_bindings) {
	bindings_ = std::move(p_bindings);
}

std::vector<UiInputConcept> UiInputRouter::concepts_of_key(std::string_view p_idk) const {
	return bindings_.concepts_of(UiInputDevice::Keyboard, p_idk);
}

std::vector<UiInputConcept> UiInputRouter::concepts_of_mouse(std::string_view p_idb) const {
	return bindings_.concepts_of(UiInputDevice::Mouse, p_idb);
}

}
