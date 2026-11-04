#include "hud_feed_tdm_parts.h"

#include "bf3/core/ui/hud_squads.h"
#include "bf3/core/util/key_values.h"

#include <algorithm>
#include <array>
#include <span>

namespace bf3::host::tdm_feed {

namespace ui = bf3::core::ui;

namespace {

uint8_t own_squad(const Frame &p_frame) {
	for (const net::PlayerRow &row : p_frame.input.players) {
		if (row.slot == p_frame.own_slot && row.squad <= net::kMaxSquads) {
			return row.squad;
		}
	}
	return 0;
}

bool own_squad_leader(const Frame &p_frame) {
	for (const net::PlayerRow &row : p_frame.input.players) {
		if (row.slot == p_frame.own_slot) {
			return row.leader;
		}
	}
	return false;
}

void add_member(ui::HudSquadMemberList &r_list, const net::PlayerRow &p_row, uint64_t &r_dropped) {
	if (r_list.count >= r_list.index.size()) {
		++r_dropped;
		return;
	}
	r_list.index[r_list.count++] = p_row.slot;
	if (p_row.leader) {
		r_list.leader = p_row.slot;
	}
}

const present::PresentedSoldier *presented(const Frame &p_frame, uint8_t p_slot) {
	for (const present::PresentedSoldier &s : p_frame.input.others) {
		if (s.slot == p_slot) {
			return &s;
		}
	}
	return nullptr;
}

}

void feed_squad(Frame &r_frame) {
	const HudFeedInput &in = r_frame.input;
	ui::HudModel &model = r_frame.model;
	HudTdmFeedState &state = r_frame.state;
	model.squad_leaders.fill(false);
	for (const net::PlayerRow &row : in.players) {
		if (row.slot >= ui::kHudMaxPlayers) {
			continue;
		}
		if (row.squad > net::kMaxSquads) {
			++state.bad_inputs;
			continue;
		}
		model.players[row.slot].squad = int8_t(row.squad);
		model.squad_leaders[row.slot] = row.squad != 0 && row.leader;
	}
	ui::HudSquad &squad = model.squad;
	squad = ui::HudSquad();
	const uint8_t mine = r_frame.own_team != 0 ? own_squad(r_frame) : uint8_t(0);
	std::array<uint8_t, net::kMaxSquads + 1> counts{};
	std::array<const net::PlayerRow *, ui::kHudMaxPlayers> mates{};
	size_t mate_count = 0;
	for (const net::PlayerRow &row : in.players) {
		if (row.team != r_frame.own_team || row.squad == 0 || row.squad > net::kMaxSquads ||
				row.slot >= ui::kHudMaxPlayers) {
			continue;
		}
		++counts[row.squad];
		if (mine != 0 && row.squad == mine && mate_count < mates.size()) {
			mates[mate_count++] = &row;
		}
	}
	std::sort(mates.begin(), mates.begin() + std::ptrdiff_t(mate_count),
			[](const net::PlayerRow *p_a, const net::PlayerRow *p_b) { return p_a->slot < p_b->slot; });
	const std::span<const net::PlayerRow *const> in_squad(mates.data(), mate_count);
	for (const net::PlayerRow *m : in_squad) {
		add_member(squad.local_squad_members, *m, state.rows_dropped);
	}
	squad.squad_members = squad.local_squad_members;
	squad.local_player_has_squad = mine != 0;
	squad.squad_name = int32_t(mine);
	squad.local_player_is_squad_leader = mine != 0 && own_squad_leader(r_frame);
	squad.squad_length = int32_t(mate_count);
	squad.squad_is_empty = mine == 0 || mate_count <= 1;
	squad.leader_in_active_squad = squad.local_player_is_squad_leader && mate_count > 1;
	for (uint8_t q = 1; q <= net::kMaxSquads; ++q) {
		if (counts[q] == 0) {
			continue;
		}
		ui::HudSquadEntry &e = squad.available_squads.squads[squad.available_squads.count++];
		e.squad = int8_t(q);
		e.members = counts[q];
		e.open = counts[q] < ui::kHudSquadSize;
	}

	const bool deploy_phase = r_frame.phase == net::MatchPhase::PreRound || r_frame.phase == net::MatchPhase::InRound;
	ui::HudSpawnPointList &list = model.spawn_points.spawn_points;
	for (const net::PlayerRow *m : in_squad) {
		if (m->slot == r_frame.own_slot) {
			continue;
		}
		if (list.count >= list.points.size()) {
			++state.rows_dropped;
			continue;
		}
		ui::HudSpawnPoint &p = list.points[list.count++];
		p.kind = ui::HudSpawnKind::SquadMate;
		p.team = int8_t(r_frame.own_team);
		p.player = m->slot;
		const present::PresentedSoldier *at = presented(r_frame, m->slot);
		p.available = r_frame.match && deploy_phase && m->alive && at != nullptr;
		if (at != nullptr) {
			p.x = at->position[0];
			p.z = at->position[2];
			p.yaw = at->yaw;
		}
	}
}

}

namespace bf3::host {

core::ui::HudSquadOrder hud_squad_order(net::SquadOrderKind p_kind) {
	switch (p_kind) {
		case net::SquadOrderKind::Move:
			return core::ui::HudSquadOrder::Move;
		case net::SquadOrderKind::Attack:
			return core::ui::HudSquadOrder::Attack;
		case net::SquadOrderKind::Defend:
			return core::ui::HudSquadOrder::Defend;
		case net::SquadOrderKind::None:
			break;
	}
	return core::ui::HudSquadOrder::None;
}

std::optional<net::SquadOrder> own_squad_order(std::span<const std::array<net::SquadOrder, net::kMaxSquads>> p_orders,
		std::span<const net::PlayerRow> p_players, uint8_t p_own_slot) {
	for (const net::PlayerRow &row : p_players) {
		if (row.slot != p_own_slot) {
			continue;
		}
		if (row.team < 1 || row.team > p_orders.size() || row.squad < 1 || row.squad > net::kMaxSquads) {
			return std::nullopt;
		}
		const net::SquadOrder &o = p_orders[row.team - 1u][row.squad - 1u];
		if (o.team != row.team || o.squad != row.squad || hud_squad_order(o.kind) == core::ui::HudSquadOrder::None) {
			return std::nullopt;
		}
		return o;
	}
	return std::nullopt;
}

std::optional<net::SquadRequest> squad_flow_request(std::span<const std::string> p_params,
		std::span<const net::PlayerRow> p_players, uint8_t p_own_slot, std::optional<std::string_view> p_selected_squad,
		std::optional<std::string_view> p_selected_player) {
	if (p_params.empty()) {
		return std::nullopt;
	}
	const net::PlayerRow *own = nullptr;
	for (const net::PlayerRow &row : p_players) {
		if (row.slot == p_own_slot) {
			own = &row;
		}
	}
	if (own == nullptr || own->team == 0) {
		return std::nullopt;
	}
	const std::string_view what = p_params.front();
	net::SquadRequest q;
	if (what == core::ui::kSquadCreateParam) {
		q.op = net::SquadOp::Create;
		return q;
	}
	if (what == core::ui::kSquadJoinParam) {
		int32_t squad = 0;
		if (!p_selected_squad ||
				core::parse_i32_value(*p_selected_squad, 1.0, double(net::kMaxSquads), squad).fault !=
						core::KeyFault::None) {
			return std::nullopt;
		}
		q.op = own->squad == squad ? net::SquadOp::Leave : net::SquadOp::Join;
		q.squad = own->squad == squad ? uint8_t(0) : uint8_t(squad);
		return q;
	}
	if (what == core::ui::kSquadKickParam && p_selected_player && own->squad != 0 && own->leader) {
		for (const net::PlayerRow &row : p_players) {
			if (row.slot != p_own_slot && row.team == own->team && row.squad == own->squad &&
					net::player_name(row) == *p_selected_player) {
				q.op = net::SquadOp::Kick;
				q.target = row.slot;
				return q;
			}
		}
	}
	return std::nullopt;
}

}
