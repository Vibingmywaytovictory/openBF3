#include "bf3/host/squad_join.h"

#include "bf3/core/math.h"
#include "bf3/core/util/format.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::host {

namespace {

const net::PlayerRow *row_of(std::span<const net::PlayerRow> p_players, uint8_t p_slot) {
	for (const net::PlayerRow &row : p_players) {
		if (row.slot == p_slot) {
			return &row;
		}
	}
	return nullptr;
}

const net::PlayerRow *row_named(std::span<const net::PlayerRow> p_players, std::string_view p_name) {
	for (const net::PlayerRow &row : p_players) {
		if (std::string_view(row.name.data(), std::min<size_t>(row.name_bytes, row.name.size())) == p_name) {
			return &row;
		}
	}
	return nullptr;
}

}

const char *squad_join_phase_name(SquadJoinPhase p_phase) {
	switch (p_phase) {
		case SquadJoinPhase::Waiting:
			return "waiting for the mate";
		case SquadJoinPhase::Switching:
			return "switching team";
		case SquadJoinPhase::Joining:
			return "joining the mate's squad";
		case SquadJoinPhase::Deploying:
			return "deploying on the mate";
		case SquadJoinPhase::Playing:
			return "playing";
		case SquadJoinPhase::Done:
			return "done";
		case SquadJoinPhase::Failed:
			return "failed";
	}
	return "?";
}

std::string squad_list_text(std::span<const net::PlayerRow> p_players) {
	std::vector<const net::PlayerRow *> rows;
	for (const net::PlayerRow &row : p_players) {
		rows.push_back(&row);
	}
	std::sort(rows.begin(), rows.end(),
			[](const net::PlayerRow *a, const net::PlayerRow *b) { return a->slot < b->slot; });
	std::string out;
	for (const net::PlayerRow *row : rows) {
		out += core::fmt("%s%u:%u/%u%s", out.empty() ? "" : " ", unsigned(row->slot), unsigned(row->team),
				unsigned(row->squad), row->leader ? "L" : "");
	}
	return out;
}

std::string players_text(std::span<const net::PlayerRow> p_players, uint8_t p_own_slot) {
	std::vector<const net::PlayerRow *> humans;
	size_t bots = 0;
	for (const net::PlayerRow &row : p_players) {
		if (row.bot) {
			++bots;
		} else {
			humans.push_back(&row);
		}
	}
	std::sort(humans.begin(), humans.end(),
			[](const net::PlayerRow *a, const net::PlayerRow *b) { return a->slot < b->slot; });
	std::string out;
	for (const net::PlayerRow *row : humans) {
		out += core::fmt("%s%s (%sslot %u)", out.empty() ? "" : ", ", std::string(net::player_name(*row)).c_str(),
				row->slot == p_own_slot ? "you, " : "", unsigned(row->slot));
	}
	return core::fmt("%zu player(s): %s; %zu bot(s)", humans.size(), out.empty() ? "none" : out.c_str(), bots);
}

SquadJoin::SquadJoin(std::string p_mate, double p_timeout_s) : mate_(std::move(p_mate)), timeout_s_(p_timeout_s) {}

void SquadJoin::fail(const std::string &p_why) {
	failure_ = p_why;
	phase_ = SquadJoinPhase::Failed;
}

bool SquadJoin::due(double p_now_s) {
	if (sent_s_ >= 0.0 && p_now_s - sent_s_ < kSquadJoinResendSeconds) {
		return false;
	}
	sent_s_ = p_now_s;
	return true;
}

SquadJoinPhase SquadJoin::frame(double p_now_s, const SquadJoinView &p_view, const SquadJoinIo &p_io) {
	if (phase_ == SquadJoinPhase::Done || phase_ == SquadJoinPhase::Failed) {
		return phase_;
	}
	if (start_s_ < 0.0) {
		start_s_ = p_now_s;
	}
	if (p_now_s - start_s_ > timeout_s_) {
		fail(core::fmt("timed out after %.0f s %s", timeout_s_, squad_join_phase_name(phase_)));
		return phase_;
	}
	const net::PlayerRow *own = row_of(p_view.players, p_view.own_slot);
	const net::PlayerRow *mate = row_named(p_view.players, mate_);
	if (p_view.own_alive && (!life_known_ || p_view.own_life != life_)) {
		life_known_ = true;
		life_ = p_view.own_life;
		++spawns_;
		const float d = p_view.mate_position
				? core::vec::length(core::vec::sub(p_view.own_position, *p_view.mate_position))
				: -1.0f;
		if (deploying_ && d >= 0.0f && d <= kSquadJoinMateMetres) {
			++mate_spawns_;
			on_mate_ = true;
			if (first_mate_m_ < 0.0f) {
				first_mate_m_ = d;
			}
		}
		deploying_ = false;
	}
	if (p_view.round_over) {
		squads_ = squad_list_text(p_view.players);
		if (mate_spawns_ > 0) {
			phase_ = SquadJoinPhase::Done;
		} else {
			fail(core::fmt("the round ended %s, before a spawn on the mate", squad_join_phase_name(phase_)));
		}
		return phase_;
	}
	if (own == nullptr || mate == nullptr || own->team == 0 || mate->team == 0) {
		phase_ = SquadJoinPhase::Waiting;
		return phase_;
	}
	mate_slot_ = mate->slot;
	own_slot_ = own->slot;
	team_ = own->team;
	squad_ = own->squad;
	if (own->team != mate->team) {
		phase_ = SquadJoinPhase::Switching;
		if (due(p_now_s)) {
			net::SquadRequest q;
			q.op = net::SquadOp::SwitchTeam;
			p_io.request(q);
			++switches_;
		}
		return phase_;
	}
	if (own->squad == 0 || own->squad != mate->squad) {
		phase_ = SquadJoinPhase::Joining;
		if (mate->squad != 0 && due(p_now_s)) {
			net::SquadRequest q;
			q.op = net::SquadOp::Join;
			q.squad = mate->squad;
			p_io.request(q);
			++joins_;
		}
		return phase_;
	}
	if (p_view.own_alive) {
		phase_ = on_mate_ ? SquadJoinPhase::Playing : SquadJoinPhase::Deploying;
		return phase_;
	}
	on_mate_ = false;
	phase_ = SquadJoinPhase::Deploying;
	if (!p_view.can_deploy) {
		++waits_screen_;
	} else if (!mate->alive) {
		++waits_mate_;
	} else if (!p_io.deploy_on(mate->slot)) {
		++waits_row_;
	} else {
		deploys_ += deploying_ ? 0u : 1u;
		deploying_ = true;
	}
	return phase_;
}

std::string SquadJoin::report() const {
	return core::fmt("%s%s%s: mate %s slot %d, own slot %u team %u squad %u; team switches %u, joins %u, deploys on "
					 "the mate %u, spawns %u, on the mate %u (first %.1f m); frames waiting on the spawn screen %u, "
					 "on the mate's life %u, on its spawn row %u; squads %s",
			squad_join_phase_name(phase_), failure_.empty() ? "" : ", ", failure_.c_str(), mate_.c_str(), mate_slot_,
			unsigned(own_slot_), unsigned(team_), unsigned(squad_), switches_, joins_, deploys_, spawns_, mate_spawns_,
			double(first_mate_m_), waits_screen_, waits_mate_, waits_row_, squads_.empty() ? "none" : squads_.c_str());
}

}
