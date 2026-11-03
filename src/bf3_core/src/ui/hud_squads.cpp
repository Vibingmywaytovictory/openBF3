#include "bf3/core/ui/hud_squads.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/util/key_values.h"
#include "ui_ebx.h"

#include <algorithm>
#include <optional>

namespace bf3::core::ui {

Result<HudSquadNames> read_squad_names(const EbxResolver &p_db) {
	const EbxPartition *part = p_db.partition_by_name(kUiSquadCompPartition);
	if (part == nullptr) {
		return make_error("squad names: " + std::string(kUiSquadCompPartition) + " does not resolve");
	}
	const EbxInstance root = part->primary();
	if (!root.data().is_a("UISquadCompData")) {
		return make_error(
				"squad names: " + std::string(kUiSquadCompPartition) + " is a " + std::string(root.type_name()));
	}
	HudSquadNames out;
	for (size_t i = 0; i < out.size(); ++i) {
		const std::string field = "Squad" + std::to_string(i + 1);
		out[i] = detail::text_of(root.data().get(field));
		if (out[i].empty()) {
			return make_error("squad names: UISquadCompData has no " + field);
		}
	}
	return out;
}

int32_t hud_selected_squad(const HudModel &p_model, const HudClientState &p_client) {
	int32_t own = 0;
	if (p_model.local_player < p_model.players.size()) {
		own = p_model.players[p_model.local_player].squad;
	}
	if (!p_client.flow_data) {
		return own;
	}
	const std::optional<std::string> v = p_client.flow_data(kUiSquadCompPartition, kSquadSelectedSquadKey);
	int32_t selected = kSquadNoSelection;
	if (!v || parse_i32_value(*v, double(kSquadNoSelection), double(kHudMaxSquads), selected).fault != KeyFault::None ||
			selected <= 0) {
		return own;
	}
	return selected;
}

uint8_t hud_selected_squad_player(const HudModel &p_model, const HudClientState &p_client) {
	if (!p_client.flow_data || p_model.local_player >= p_model.players.size()) {
		return kHudNoPlayer;
	}
	const std::optional<std::string> name = p_client.flow_data(kUiSquadCompPartition, kSquadSelectedPlayerKey);
	const HudPlayer &local = p_model.players[p_model.local_player];
	if (!name || name->empty() || local.squad == 0) {
		return kHudNoPlayer;
	}
	for (size_t slot = 0; slot < std::min<size_t>(p_model.player_count, p_model.players.size()); ++slot) {
		const HudPlayer &p = p_model.players[slot];
		if (p.squad == local.squad && p.team == local.team && hud_player_name(p_model, uint8_t(slot)) == *name) {
			return uint8_t(slot);
		}
	}
	return kHudNoPlayer;
}

}
