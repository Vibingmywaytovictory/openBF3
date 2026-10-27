#include "hud_tdm.h"

#include "bf3/core/ui/hud_screen.h"

#include <string>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

bool tdm_hud_reports(std::string_view) {
	return false;
}

std::optional<UiScreenRecord> tdm_hud_screen_record(const TdmHudRecords &, std::string_view) {
	return std::nullopt;
}

UiLookupRecord tdm_hud_lookup_record(const TdmHudRecords &p_records) {
	return UiLookupRecord{ p_records.model_serial, p_records.draws, p_records.missing_fonts, p_records.missing_icons, p_records.missing_strings };
}

}
