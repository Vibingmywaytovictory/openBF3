#pragma once

#include "hud_widget_support.h"

#include <array>
#include <optional>
#include <string_view>

namespace bf3::core::ui {

inline constexpr std::string_view kUiCustomizationCategory = "ui/uicomponents/uicustomizationcomp";
inline constexpr int32_t kCustomizeKitsSetupKey = 537447676;
inline constexpr std::array<int32_t, kHudSpawnKitItems> kCustomizeItemSetupKeys{ 892395121, -1698501497, -167212006,
	-167212007, 175312528 };
inline constexpr size_t kCustomizeGadget1Item = 2;
inline constexpr int32_t kCustomizeWeaponSetupKey = 283830368;
inline constexpr std::array<int32_t, kHudAccessorySlots> kCustomizeAccessorySetupKeys{ -1858312134, -1858312135,
	-1858312136, -1858312129 };

std::optional<As2Value> customize_kit_setup(const HudModel &p_model, const HudClientState &p_client, int32_t p_key);

}
