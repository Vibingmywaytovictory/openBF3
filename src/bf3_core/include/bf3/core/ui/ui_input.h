#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr std::string_view kUiInputConceptsPartition = "input/defaultinputconcepts";
inline constexpr size_t kUiInputMaxBindings = 4096;

enum class UiInputConcept : int32_t {
	NavigateUp = 0,
	NavigateDown = 1,
	NavigateLeft = 2,
	NavigateRight = 3,
	TabLeft = 4,
	TabRight = 5,
	Activate = 6,
	Deactivate = 7,
	Menu = 8,
	Cancel = 9,
	OK = 10,
	Back = 11,
	Tab = 12,
	Edit = 13,
	View = 14,
	LThumb = 15,
	RThumb = 16,
	MapZoom = 17,
	MapSize = 18,
	SayAllChat = 19,
	TeamChat = 20,
	SquadChat = 21,
	Com = 22,
};

inline constexpr int32_t kUiInputConceptCount = 23;

bool input_is_bound(std::string_view p_input);
std::string_view ui_input_concept_name(UiInputConcept p_concept);
std::optional<UiInputConcept> ui_input_concept_of_action(std::string_view p_ui_input_action);
std::string_view ui_input_concept_identifier(UiInputConcept p_concept);

int32_t ui_flash_key_code(std::string_view p_idk);

enum class UiInputDevice : uint8_t {
	Keyboard,
	Mouse,
};

struct UiKeyBinding {
	std::string ui_concept;
	UiInputDevice device = UiInputDevice::Keyboard;
	std::string key;
	bool operator==(const UiKeyBinding &) const = default;
};

struct UiInputBindings {
	std::vector<UiKeyBinding> bindings;

	std::vector<UiInputConcept> concepts_of(UiInputDevice p_device, std::string_view p_key) const;
	std::vector<std::string> keys_of(std::string_view p_concept) const;
};

Result<UiInputBindings> read_ui_input_bindings(
		const EbxResolver &p_db, std::string_view p_partition = kUiInputConceptsPartition);

class UiInputRouter {
public:
	explicit UiInputRouter(UiInputBindings p_bindings = {});

	void set_bindings(UiInputBindings p_bindings);
	const UiInputBindings &bindings() const { return bindings_; }
	std::vector<UiInputConcept> concepts_of_key(std::string_view p_idk) const;
	std::vector<UiInputConcept> concepts_of_mouse(std::string_view p_idb) const;

private:
	UiInputBindings bindings_;
};

}
