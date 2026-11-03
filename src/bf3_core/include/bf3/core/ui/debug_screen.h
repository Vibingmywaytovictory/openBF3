#pragma once

#include "bf3/core/ui/hud_screen.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bf3::core::ui {

inline constexpr std::string_view kDebugScreenMovie = "ui/assets/debugscreen";
inline constexpr std::string_view kDebugFieldWidget = "ui/assets/debugfield";
inline constexpr size_t kDebugScreenFields = 72;
inline constexpr uint32_t kDebugFieldRgb = 0xFF0000;

struct DebugFieldText {
	std::string label;
	std::string value;
};

std::string debug_field_path(size_t p_index);
HudScreenAsset debug_screen_asset(std::span<const DebugFieldText> p_fields);
size_t debug_screen_show(HudScreen &r_screen, std::span<const DebugFieldText> p_fields);

}
