#include "hud_tdm.h"

#include "bf3/core/ui/hud_screen.h"

#include <array>
#include <cstring>
#include <string>

namespace bf3::godot_side {

namespace ui = bf3::core::ui;

namespace {

constexpr std::array<std::string_view, 5> kTdmHudPartitions{ ui::kScoreboardHud32ScreenPartition, ui::kSpawnScreenPcPartition, ui::kKillScreenPartition,
	ui::kPreRoundWaitingScreenPartition, ui::kEorWinningTeamScreenPartition };

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
	if (p_partition == ui::kScoreboardHud32ScreenPartition) {
		r.visible = p_records.scoreboard_visible;
		r.numbers.emplace(kUiFactRows, double(p_records.scoreboard_rows));
		r.numbers.emplace(kUiFactTeam1Kills, double(p_records.scoreboard_team_kills[0]));
		r.numbers.emplace(kUiFactTeam2Kills, double(p_records.scoreboard_team_kills[1]));
	} else if (p_partition == ui::kSpawnScreenPcPartition) {
		r.visible = p_records.spawn_screen_visible;
		r.shown = p_records.spawn_screen_shown;
		r.numbers.emplace(kUiFactDeployEnabled, p_records.deploy_enabled ? 1.0 : 0.0);
		r.numbers.emplace(kUiFactSelectedKit, double(p_records.selected_kit));
	} else if (p_partition == ui::kKillScreenPartition) {
		r.visible = p_records.kill_card_visible;
		r.numbers.emplace(kUiFactItem, double(p_records.kill_card_item));
		r.numbers.emplace(kUiFactWeaponNamed, p_records.kill_card_weapon_named ? 1.0 : 0.0);
		const ui::HudPlayerName &killer = p_records.kill_card_killer;
		r.texts.emplace(kUiFactKiller, std::string(killer.data(), strnlen(killer.data(), killer.size())));
	} else if (p_partition == ui::kPreRoundWaitingScreenPartition) {
		r.visible = p_records.pre_round_visible;
	} else if (p_partition == ui::kEorWinningTeamScreenPartition) {
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
