#include "hud_tdm.h"

#include "bf3/core/ui/hud_screen.h"

#include <array>
#include <string>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

namespace {

constexpr std::array<std::string_view, 1> kTdmHudPartitions{ ui::kEorWinningTeamScreenPartition };
}

bool tdm_hud_reports(std::string_view p_partition) {
	for (const std::string_view p : kTdmHudPartitions) {
		if (p == p_partition) {
			return true;
		}
	}
	return false;
}

std::optional<UiScreenRecord> tdm_hud_screen_record(const TdmHudRecords &p_records, std::string_view p_partition) {
	UiScreenRecord r;
	if (p_partition == ui::kEorWinningTeamScreenPartition) {
		r.visible = p_records.round_end_visible;
		r.shown = p_records.round_end_shown;
	} else {
		return std::nullopt;
	}
	return r;
}

UiLookupRecord tdm_hud_lookup_record(const TdmHudRecords &p_records) {
	return UiLookupRecord{ p_records.model_serial, p_records.draws, p_records.missing_fonts, p_records.missing_icons, p_records.missing_strings };
}

}
