#include "level_session_tdm.h"

#include "godot_util.h"

#include "bf3/core/ui/hud_squads.h"
#include "bf3/host/ui_flow_session.h"

#include <godot_cpp/variant/utility_functions.hpp>

namespace bf3::godot_side {

namespace host = bf3::host;
namespace net = bf3::net;
namespace ui = bf3::core::ui;

namespace {

constexpr size_t kMaxQueuedSquadRequests = 16;

void queue(TdmState &r_m, const net::SquadRequest &p_request) {
	if (r_m.squad_requests.size() < kMaxQueuedSquadRequests) {
		r_m.squad_requests.push_back(p_request);
	}
}

}

void load_squad_names(TdmState &r_m, const bf3::core::EbxResolver *p_resolver) {
	if (p_resolver == nullptr || r_m.squad_names_loaded) {
		return;
	}
	r_m.squad_names_loaded = true;
	if (bf3::core::Result<ui::HudSquadNames> names = ui::read_squad_names(*p_resolver)) {
		r_m.squad_names = std::move(*names);
	} else {
		godot::UtilityFunctions::print(to_godot("[ui] ERROR " + names.error().message));
	}
}

bool take_squad_action(TdmState &r_m, const ui::UiFlowAction &p_action, const host::PresentationFrame &p_f,
		const host::UiFlowSession &p_session) {
	if (p_action.key == ui::kSwitchTeamFlowActionKey) {
		net::SquadRequest q;
		q.op = net::SquadOp::SwitchTeam;
		queue(r_m, q);
		return true;
	}
	if (p_action.key != ui::kSquadFlowActionKey) {
		return false;
	}
	const std::span<const net::PlayerRow> players(
			p_f.players.data(), std::min<size_t>(p_f.player_count, p_f.players.size()));
	const std::optional<std::string> squad = p_session.data(ui::kUiSquadCompPartition, ui::kSquadSelectedSquadKey);
	const std::optional<std::string> player = p_session.data(ui::kUiSquadCompPartition, ui::kSquadSelectedPlayerKey);
	if (const std::optional<net::SquadRequest> q = host::squad_flow_request(p_action.params, players, r_m.own_slot,
				squad ? std::optional<std::string_view>(*squad) : std::nullopt,
				player ? std::optional<std::string_view>(*player) : std::nullopt)) {
		queue(r_m, *q);
	}
	return true;
}

uint8_t tdm_spawn_choice(const ui::HudModel &p_model, const host::PresentationFrame &p_f, uint8_t p_own_slot) {
	const ui::HudSpawnPointList &list = p_model.spawn_points.spawn_points;
	const int32_t selected = p_model.spawn_points.selected_point;
	if (selected < 0 || selected >= int32_t(std::min<size_t>(list.count, list.points.size()))) {
		return 0;
	}
	const ui::HudSpawnPoint &p = list.points[size_t(selected)];
	if (p.kind != ui::HudSpawnKind::SquadMate || !p.available) {
		return 0;
	}
	const std::span<const net::PlayerRow> players(
			p_f.players.data(), std::min<size_t>(p_f.player_count, p_f.players.size()));
	return host::squad_spawn_choice(players, p_own_slot, p.player);
}

void send_squad_requests(TdmState &r_m, host::ClientSession &r_client) {
	for (const net::SquadRequest &q : r_m.squad_requests) {
		r_client.send_squad_request(q);
		++r_m.squad_requests_sent;
	}
	r_m.squad_requests.clear();
}

}
