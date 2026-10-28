#pragma once

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace bf3::core::ui {

std::string as2_service_stars_text(double p_stars);

class KitIconsWidget final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override;

	void set_stars(double p_stars) const;

	const As2Clip &service_stars() const { return service_stars_; }

private:
	As2Clip service_stars_;
};

class ServiceStarsWidget final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override;

	void set_stars(double p_stars) const;
	float width() const;

	const As2Clip &field() const { return txt_; }

private:
	As2Clip txt_;
};

struct GameUtilsKit {
	std::string_view us;
	std::string_view ru;
	std::string_view sid;
	std::string_view label;
};

inline constexpr std::array<GameUtilsKit, kHudKitCount> kGameUtilsKits{ {
		{ "USAssault", "RUAssault", "ID_M_ASSAULT", "assault" },
		{ "USEngineer", "RUEngineer", "ID_M_ENGINEER", "engineer" },
		{ "USMedic", "RUMedic", "ID_M_SUPPORT", "medic" },
		{ "USRecon", "RURecon", "ID_M_RECON", "recon" },
} };

std::string game_utils_kit_frame_label(std::string_view p_kit);
bool game_utils_is_image_path(const As2Value &p_path);
As2Value game_utils_string2object(std::string_view p_text);
std::string game_utils_object2string(const As2Value &p_object);
std::optional<As2Value> ui_base_object_to_array(const As2Value &p_object);

}
