#include "bf3/host/squads.h"

#include <algorithm>
#include <utility>

namespace bf3::host {

namespace {

bool playing_team(uint8_t p_team) {
	return p_team == 1 || p_team == 2;
}

bool squad_id(uint8_t p_squad) {
	return p_squad >= 1 && p_squad <= net::kMaxSquads;
}

const SquadOrderState kNoOrder{};

}

void SquadTable::configure(size_t p_slots, uint8_t p_squad_size) {
	size_ = p_squad_size == 0 ? kDefaultSquadSize : p_squad_size;
	members_.assign(p_slots, Member());
	for (auto &team : squads_) {
		for (Squad &s : team) {
			s = Squad();
		}
	}
	++revision_;
}

void SquadTable::clear() {
	configure(members_.size(), size_);
}

SquadTable::Squad *SquadTable::find(uint8_t p_team, uint8_t p_squad) {
	return playing_team(p_team) && squad_id(p_squad) ? &squads_[p_team - 1u][p_squad - 1u] : nullptr;
}

const SquadTable::Squad *SquadTable::find(uint8_t p_team, uint8_t p_squad) const {
	return playing_team(p_team) && squad_id(p_squad) ? &squads_[p_team - 1u][p_squad - 1u] : nullptr;
}

uint8_t SquadTable::squad_of(uint8_t p_slot) const {
	return p_slot < members_.size() ? members_[p_slot].squad : 0;
}

uint8_t SquadTable::team_of(uint8_t p_slot) const {
	return p_slot < members_.size() ? members_[p_slot].team : 0;
}

bool SquadTable::leader(uint8_t p_slot) const {
	const uint8_t squad = squad_of(p_slot);
	const Squad *s = find(team_of(p_slot), squad);
	return s != nullptr && !s->members.empty() && s->members.front() == p_slot;
}

bool SquadTable::same_squad(uint8_t p_a, uint8_t p_b) const {
	return p_a != p_b && squad_of(p_a) != 0 && squad_of(p_a) == squad_of(p_b) && team_of(p_a) == team_of(p_b);
}

std::span<const uint8_t> SquadTable::members(uint8_t p_team, uint8_t p_squad) const {
	const Squad *s = find(p_team, p_squad);
	return s != nullptr ? std::span<const uint8_t>(s->members) : std::span<const uint8_t>();
}

int SquadTable::mate(uint8_t p_slot, uint8_t p_index) const {
	const std::span<const uint8_t> in = members(team_of(p_slot), squad_of(p_slot));
	if (p_index >= in.size()) {
		return kNoSquadMate;
	}
	std::vector<uint8_t> by_slot(in.begin(), in.end());
	std::sort(by_slot.begin(), by_slot.end());
	return int(by_slot[p_index]);
}

bool SquadTable::is_private(uint8_t p_team, uint8_t p_squad) const {
	const Squad *s = find(p_team, p_squad);
	return s != nullptr && s->privacy;
}

const SquadOrderState &SquadTable::order(uint8_t p_team, uint8_t p_squad) const {
	const Squad *s = find(p_team, p_squad);
	return s != nullptr ? s->order : kNoOrder;
}

void SquadTable::put(uint8_t p_slot, uint8_t p_team, uint8_t p_squad) {
	Squad *s = find(p_team, p_squad);
	if (s == nullptr || p_slot >= members_.size()) {
		return;
	}
	s->members.push_back(p_slot);
	members_[p_slot] = Member{ p_team, p_squad };
	++revision_;
}

uint8_t SquadTable::first_empty(uint8_t p_team) const {
	for (uint8_t q = 1; q <= net::kMaxSquads; ++q) {
		if (find(p_team, q)->members.empty()) {
			return q;
		}
	}
	return 0;
}

uint8_t SquadTable::assign(uint8_t p_slot, uint8_t p_team) {
	if (p_slot >= members_.size()) {
		return 0;
	}
	if (members_[p_slot].team == p_team && members_[p_slot].squad != 0) {
		return members_[p_slot].squad;
	}
	remove(p_slot);
	members_[p_slot].team = playing_team(p_team) ? p_team : 0;
	if (!playing_team(p_team)) {
		return 0;
	}
	uint8_t chosen = 0;
	for (uint8_t q = 1; q <= net::kMaxSquads && chosen == 0; ++q) {
		const size_t n = find(p_team, q)->members.size();
		if (n > 0 && n < size_) {
			chosen = q;
		}
	}
	if (chosen == 0) {
		chosen = first_empty(p_team);
	}
	if (chosen != 0) {
		put(p_slot, p_team, chosen);
	}
	return chosen;
}

void SquadTable::remove(uint8_t p_slot) {
	if (p_slot >= members_.size()) {
		return;
	}
	Member &m = members_[p_slot];
	if (Squad *s = find(m.team, m.squad); s != nullptr) {
		std::erase(s->members, p_slot);
		if (s->members.empty()) {
			s->privacy = false;
			s->order = SquadOrderState();
		}
		++revision_;
	}
	m.squad = 0;
	m.team = 0;
}

void SquadTable::follow_team(uint8_t p_slot, uint8_t p_team) {
	if (p_slot >= members_.size() || members_[p_slot].team == p_team) {
		return;
	}
	assign(p_slot, p_team);
}

void SquadTable::swap_teams() {
	std::swap(squads_[0], squads_[1]);
	for (Member &m : members_) {
		if (playing_team(m.team)) {
			m.team = uint8_t(3u - m.team);
		}
	}
	++revision_;
}

SquadRefusal SquadTable::join(uint8_t p_slot, uint8_t p_team, uint8_t p_squad) {
	if (p_slot >= members_.size() || !playing_team(p_team)) {
		return SquadRefusal::NoTeam;
	}
	const Squad *s = find(p_team, p_squad);
	if (s == nullptr) {
		return SquadRefusal::BadSquad;
	}
	if (members_[p_slot].team == p_team && members_[p_slot].squad == p_squad) {
		return SquadRefusal::SameSquad;
	}
	if (s->members.empty()) {
		return SquadRefusal::Empty;
	}
	if (s->members.size() >= size_) {
		return SquadRefusal::Full;
	}
	remove(p_slot);
	put(p_slot, p_team, p_squad);
	return SquadRefusal::None;
}

SquadRefusal SquadTable::create(uint8_t p_slot, uint8_t p_team) {
	if (p_slot >= members_.size() || !playing_team(p_team)) {
		return SquadRefusal::NoTeam;
	}
	const uint8_t q = first_empty(p_team);
	if (q == 0) {
		return SquadRefusal::Full;
	}
	remove(p_slot);
	put(p_slot, p_team, q);
	return SquadRefusal::None;
}

SquadRefusal SquadTable::leave(uint8_t p_slot) {
	if (squad_of(p_slot) == 0) {
		return SquadRefusal::NotMember;
	}
	const uint8_t team = members_[p_slot].team;
	remove(p_slot);
	members_[p_slot].team = team;
	return SquadRefusal::None;
}

SquadRefusal SquadTable::kick(uint8_t p_leader, uint8_t p_target) {
	if (p_leader == p_target) {
		return SquadRefusal::Self;
	}
	if (!leader(p_leader)) {
		return SquadRefusal::NotLeader;
	}
	if (!same_squad(p_leader, p_target)) {
		return SquadRefusal::NotMember;
	}
	return leave(p_target);
}

SquadRefusal SquadTable::set_private(uint8_t p_leader, bool p_private) {
	if (!leader(p_leader)) {
		return SquadRefusal::NotLeader;
	}
	find(team_of(p_leader), squad_of(p_leader))->privacy = p_private;
	++revision_;
	return SquadRefusal::None;
}

bool SquadTable::set_order(uint8_t p_team, uint8_t p_squad, const SquadOrderState &p_order) {
	Squad *s = find(p_team, p_squad);
	if (s == nullptr || s->members.empty() || uint8_t(p_order.kind) > uint8_t(net::SquadOrderKind::Defend)) {
		return false;
	}
	const SquadOrderState &was = s->order;
	if (was.kind == p_order.kind && was.area == p_order.area && was.position[0] == p_order.position[0] &&
			was.position[1] == p_order.position[1] && was.position[2] == p_order.position[2]) {
		return true;
	}
	s->order = p_order;
	++revision_;
	return true;
}

}
